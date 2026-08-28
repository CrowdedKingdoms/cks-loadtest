#!/usr/bin/env bash
# Mint identity sessions for a bot population OUT OF BAND, and write them to a
# roster file the harness consumes with LT_ROSTER_FILE.
#
# WHY THIS EXISTS: AUTHENTICATION IS PART OF THE MEASUREMENT.
#
# The API hashes passwords with bcrypt at 10 rounds, which costs 0.4-1.4s of
# server CPU per sign-in. A hundred clients signing in at run start therefore
# burn API CPU in exactly the first seconds where the numbers matter, and the
# result is a measurement partly of the harness's own login storm. Minting the
# sessions beforehand moves that cost outside the run; the harness then reports
# `0 session(s) minted DURING the run`, which is the claim a reader needs.
#
# Sessions last 30 days, so one roster serves many runs.
#
# THIS USES ONLY PUBLIC MUTATIONS -- `login` and `register`, exactly what any
# customer of the platform can call. It needs no operator, no super admin and no
# access to the hosting infrastructure. That is deliberate: the harness is
# intended to be usable by a tenant against their own app, and a provisioning
# step that required privilege would make it unusable published.
#
# WHAT MUST ALREADY BE TRUE (this script does not do any of it):
#   - the app exists and is configured
#   - the organization owning it is funded, or exempt from billing
#   - the bot accounts are entitled to the app, if it is not open to all
# Funding and entitlement are deliberately out of scope: they need privileges a
# tenant holds for their own app and a load test has no business asserting.
#
#   LT_MANAGEMENT_API_URL=https://<origin> \
#   LT_EMAIL=bots@example.invalid \
#   LT_CLIENTS=100 \
#   LT_PASSWORD='...' \
#   scripts/provision-roster.sh
#
# Passwords, one of two ways:
#   LT_PASSWORD            one shared password for every derived account
#   LT_PASSWORD_HMAC_SEED  per-account password = Aa1!<HMAC-SHA256-base64url of
#                          "crowdy-loadbot-v1:<email>" under the seed>. Prefer
#                          this for a large population: a leaked single password
#                          is a leak of the whole roster, and the seed lets a
#                          password be recomputed without being stored.
#
# The roster records the ORIGIN it was minted against and the harness refuses a
# roster from a different one. A session minted on one tier is a syntactically
# valid bearer token that means nothing on another, so a file carried between
# tiers produces authentication failures that read like a broken tier.

set -euo pipefail

API="${LT_MANAGEMENT_API_URL:-}"
EMAIL="${LT_EMAIL:-}"
CLIENTS="${LT_CLIENTS:-10}"
# NOT `${LT_EMAIL_PATTERN:-{local}+...}`: the first `}` inside the default
# closes the parameter expansion, so the default silently becomes `{local` and
# every derived address comes out malformed. Assign the fallback separately.
PATTERN="${LT_EMAIL_PATTERN:-}"
[ -n "$PATTERN" ] || PATTERN='{local}+lt-{index}@{domain}'
OUT="${LT_ROSTER_FILE:-loadtest-roster.json}"
SEED="${LT_PASSWORD_HMAC_SEED:-}"
PASSWORD="${LT_PASSWORD:-}"

die() { echo "error: $*" >&2; exit 2; }

[ -n "$API" ] || die "LT_MANAGEMENT_API_URL is required"
[ -n "$EMAIL" ] || die "LT_EMAIL is required (the pattern derives from it)"
case "$EMAIL" in *@*) ;; *) die "LT_EMAIL must be a full email address" ;; esac
if [ -z "$SEED" ] && [ -z "$PASSWORD" ]; then
  die "set LT_PASSWORD or LT_PASSWORD_HMAC_SEED"
fi
for tool in curl jq openssl; do
  command -v "$tool" >/dev/null || die "$tool is required"
done

API="${API%/}"
LOCAL="${EMAIL%@*}"
DOMAIN="${EMAIL#*@}"

# Same derivation as the harness's Config::derivedEmail, so the roster names the
# accounts the run will look for. A disagreement here is caught by the harness
# rather than tolerated, but agreeing in the first place is cheaper.
derived_email() {
  local idx padded out
  idx="$1"
  padded=$(printf '%04d' "$idx")
  out="${PATTERN//\{local\}/$LOCAL}"
  out="${out//\{domain\}/$DOMAIN}"
  out="${out//\{index\}/$padded}"
  printf '%s' "$out"
}

# HMAC-SHA256 base64url, matching the JS `digest("base64url")`: base64 with
# +/ mapped to -_ and padding stripped.
derived_password() {
  local email mac
  email="$1"
  if [ -z "$SEED" ]; then printf '%s' "$PASSWORD"; return; fi
  mac=$(printf 'crowdy-loadbot-v1:%s' "$email" \
        | openssl dgst -sha256 -hmac "$SEED" -binary \
        | openssl base64 -A \
        | tr '+/' '-_' | tr -d '=')
  printf 'Aa1!%s' "$mac"
}

gql() { # query variables-json
  curl -sS --max-time 60 --max-redirs 0 --proto '=https,http' "$API/graphql" \
    -H 'Content-Type: application/json' \
    --data-binary "$(jq -cn --arg q "$1" --argjson v "$2" '{query:$q,variables:$v}')"
}

Q_LOGIN='mutation L($e:String!,$p:String!){ login(loginUserInput:{email:$e,password:$p}){ token user { userId } } }'
# `register` returns an AuthResponse with a usable `token`, so a brand-new
# account needs no follow-up login. Matches src/Provisioner.cpp exactly; the
# argument is `registerUserInput`, and guessing `createUserInput` here failed
# validation while the script reported the LOGIN error, which named the wrong
# thing entirely.
Q_REGISTER='mutation R($e:String!,$p:String!){ register(registerUserInput:{email:$e,password:$p}){ token user { userId } } }'

echo "roster: $CLIENTS account(s) against $API"
echo "  password source: $([ -n "$SEED" ] && echo 'HMAC-derived per account' || echo 'one shared LT_PASSWORD')"

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
: > "$tmp"

minted=0; registered=0; failed=0
for i in $(seq 0 $((CLIENTS - 1))); do
  em=$(derived_email "$i")
  pw=$(derived_password "$em")
  vars=$(jq -cn --arg e "$em" --arg p "$pw" '{e:$e,p:$p}')

  resp=$(gql "$Q_LOGIN" "$vars")
  token=$(printf '%s' "$resp" | jq -r '.data.login.token // empty')

  login_err=$(printf '%s' "$resp" | jq -r '.errors[0].message // empty')
  reg_err=""
  if [ -z "$token" ]; then
    # Unregistered is the expected first-run state, and `register` hands back a
    # session, so there is no second login to do.
    reg=$(gql "$Q_REGISTER" "$vars")
    token=$(printf '%s' "$reg" | jq -r '.data.register.token // empty')
    if [ -n "$token" ]; then
      registered=$((registered + 1))
    else
      reg_err=$(printf '%s' "$reg" | jq -r '.errors[0].message // "no token and no error"')
    fi
  fi

  if [ -z "$token" ]; then
    failed=$((failed + 1))
    # Report BOTH refusals. Printing only the login error is how a malformed
    # register mutation gets reported as "Invalid credentials" -- a message about
    # the password, for a request the server rejected before looking at one.
    echo "  $i $em FAILED" >&2
    echo "      login:    ${login_err:-<no error, but no token>}" >&2
    echo "      register: ${reg_err}" >&2
    continue
  fi

  minted=$((minted + 1))
  jq -cn --argjson i "$i" --arg e "$em" --arg t "$token" \
     '{index:$i,email:$e,token:$t}' >> "$tmp"
  if [ $((minted % 25)) -eq 0 ]; then echo "  ...$minted/$CLIENTS"; fi
done

jq -s --arg origin "$API" --arg at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
   '{origin:$origin, mintedAt:$at, sessions:.}' "$tmp" > "$OUT.partial"
mv "$OUT.partial" "$OUT"
chmod 600 "$OUT"

echo "wrote $OUT ($minted session(s), $registered newly registered, $failed failed)"
echo "  mode 0600: these are live bearer tokens."
echo
echo "run with:"
echo "  LT_ROSTER_FILE=$OUT LT_ROSTER_REQUIRED=1 ..."

# A short roster is a FAILURE of this script, not a partial success. The harness
# can be told to refuse one, but a provisioning step that exits 0 having minted
# 60 of 100 sessions is how a bcrypt convoy survives into a run nobody suspects.
[ "$failed" -eq 0 ] || exit 1
