# Security policy

## Reporting a vulnerability

Email **hello@crowdedkingdoms.com** with the subject line starting `SECURITY:` and a description, the affected
component and version (a git tag, an npm version, or the URL of the environment),
and steps to reproduce. Do not open a public issue for a vulnerability. You will
get an acknowledgement within two business days and a status update at least
every week until resolution.

Please do not test against `prod` (`*.prod.crowdedkingdoms.com`, `crowdy.games`)
without asking first. The `dev` tier is where we would rather you found things.

## Supported tiers

Every service runs three tiers from three long-lived branches: `dev`, `test`,
`prod`. A fix lands on `dev` and is promoted forward; only the current `prod`
release of each component is supported.

## What is in scope

- The Crowded Kingdoms API (`ck.<tier>.crowdedkingdoms.com`): authentication,
  authorization, the portal / hosted sign-in flow, tenant isolation between
  organizations and apps, billing correctness.
- Crowded Kingdoms Studio (`studio.<tier>.crowdedkingdoms.com`).
- The SDKs (CrowdyJS, CrowdyCPP) and the public starter (The Construct).
- The realtime replication service, as reachable through a published client.

## How this repository is guarded

`.github/workflows/security.yml` runs gitleaks (blocking), a dependency audit
and static analysis on every pull request and weekly; Dependabot alerts are on.
Changes to sign-in, token or permission code go through a security review
before they are opened (see `AGENTS.md`). The identity threat model and
incident runbook live with the platform documentation.
