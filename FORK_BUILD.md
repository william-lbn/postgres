# PostgreSQL source fork

`main` is the upstream explanatory branch, not PostgreSQL source. Database code
is in `REL_14_STABLE_neon`, `REL_15_STABLE_neon`, `REL_16_STABLE_neon`,
`REL_17_STABLE_neon` and `REL_18_STABLE_neon`. The initial branch restoration
preserves upstream commits without editing the database source.

The workflow on `main` compiles and tests all five branch heads on native Linux
amd64 and publishes binaries as Actions artifacts. Dispatch it after updating
database branches. Production Neon images are built by `william-lbn/neon`, using
the exact commits in Neon's git submodules and `ci/distribution.json`; these
may differ from the latest branch heads. PG18 is not enabled in the current Neon
distribution merely because its PostgreSQL branch exists and compiles.

Keep the upstream repository and its license history. Upgrade and test the Neon
gitlinks deliberately instead of replacing them with the newest PostgreSQL
branch heads without integration tests.
