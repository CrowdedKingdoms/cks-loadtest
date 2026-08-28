#!/usr/bin/env bash
# Owner-side helper: entitle the load tester's derived accounts to an app tier.
#
# The load tester itself is tier-agnostic and never manages entitlements. If
# your app's default tier does not carry the runtime permissions (at minimum
# "access"), Buddy rejects the simulated clients' spatial traffic with
# UNAUTHORIZED. Run this once as an app owner (a user with the
# 'manage_access_tiers' permission on the app) to grant every derived account
# a permission-bearing tier.
#
# The derived accounts must already exist (run cks-loadtest once first, or
# this script's login step will fail for missing accounts).
#
# Required env:
#   LT_EMAIL, LT_PASSWORD, LT_MANAGEMENT_API_URL, LT_APP_ID, LT_CLIENTS
#   OWNER_EMAIL                 owner account email
#   OWNER_PASSWORD              owner password
#   TIER_ID                     tier to grant (see appAccessTiers)
# Optional:
#   LT_EMAIL_PATTERN            default '{local}+lt-{index}@{domain}'
set -euo pipefail

: "${LT_EMAIL:?}" "${LT_PASSWORD:?}" "${LT_MANAGEMENT_API_URL:?}" "${LT_APP_ID:?}" "${LT_CLIENTS:?}"
: "${OWNER_EMAIL:?}" "${TIER_ID:?}"

GQL="${LT_MANAGEMENT_API_URL%/}/graphql"
DEFAULT_PATTERN='{local}+lt-{index}@{domain}'
PATTERN="${LT_EMAIL_PATTERN:-$DEFAULT_PATTERN}"
LOCAL="${LT_EMAIL%%@*}"
DOMAIN="${LT_EMAIL##*@}"

gql() { # gql <query> <variables-json> [bearer]
  local auth=()
  [[ -n "${3:-}" ]] && auth=(-H "Authorization: Bearer $3")
  curl -fsS --max-redirs 0 --proto '=https,http' "$GQL" -H 'Content-Type: application/json' "${auth[@]}" \
    -d "$(jq -cn --arg q "$1" --argjson v "$2" '{query:$q, variables:$v}')"
}

fail_on_errors() { # fail_on_errors <response> <context>
  if jq -e '.errors' <<<"$1" >/dev/null 2>&1; then
    echo "error ($2): $(jq -c '.errors[0].message' <<<"$1")" >&2
    return 1
  fi
}

# One way in. OWNER_DEV_LOGIN=1 used to select `devLogin` here, which issued a
# session for any address with no proof of ownership; it is deleted from ck-api
# and the password branch below was already the other half of this `if`.
echo "signing in owner $OWNER_EMAIL..."
: "${OWNER_PASSWORD:?set OWNER_PASSWORD (OWNER_DEV_LOGIN is gone: the bypass was removed from every tier)}"
resp=$(gql 'mutation($i: LoginUserInput!){ login(loginUserInput:$i){ token } }' \
           "$(jq -cn --arg e "$OWNER_EMAIL" --arg p "$OWNER_PASSWORD" '{i:{email:$e,password:$p}}')")
fail_on_errors "$resp" "owner login"
OWNER_TOKEN=$(jq -r '.data.login.token' <<<"$resp")

granted=0
for ((i = 0; i < LT_CLIENTS; i++)); do
  idx=$(printf '%04d' "$i")
  email="${PATTERN//\{local\}/$LOCAL}"
  email="${email//\{domain\}/$DOMAIN}"
  email="${email//\{index\}/$idx}"

  resp=$(gql 'mutation($i: LoginUserInput!){ login(loginUserInput:$i){ user { userId } } }' \
             "$(jq -cn --arg e "$email" --arg p "$LT_PASSWORD" '{i:{email:$e,password:$p}}')")
  fail_on_errors "$resp" "login $email (has cks-loadtest created the accounts yet?)"
  user_id=$(jq -r '.data.login.user.userId' <<<"$resp")

  resp=$(gql 'mutation($i: GrantAppAccessInput!){ grantAppAccess(input:$i){ status tierId } }' \
             "$(jq -cn --arg a "$LT_APP_ID" --arg u "$user_id" --arg t "$TIER_ID" '{i:{appId:$a,userId:$u,tierId:$t}}')" \
             "$OWNER_TOKEN")
  fail_on_errors "$resp" "grantAppAccess $email"
  granted=$((granted + 1))
done

echo "granted tier $TIER_ID on app $LT_APP_ID to $granted derived account(s)."
