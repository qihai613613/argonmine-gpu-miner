# Security policy

## Scope

This is a **solo miner**. It never needs your private key, recovery phrase, or
wallet password. The only user input it needs is a public `arg1...` payout
address.

The miner verifies every work template before hashing it:

- the coinbase pays the payout address
- the treasury receives its consensus share (from block 300)
- the coinbase commits to the header's transaction and witness roots
- the header's `bits` match the advertised target

Do not use `--no-verify` except while debugging a work server you control.

## Reporting a vulnerability

Please report security issues through GitHub's **Security → Report a
vulnerability** flow (private advisory), not in a public issue.

Include:

- the affected version/commit
- steps to reproduce
- a description of the impact
- whether the issue affects consensus code, template verification, the
  dashboard, or the build

## Key safety

- Never paste a 24-word recovery phrase or private key into the miner, the
  dashboard, an issue, or a chat.
- The dashboard only stores public addresses and a manually entered hashrate
  in the browser's `localStorage`.
- The optional local dashboard proxy binds to `127.0.0.1` by default and only
  forwards the explorer endpoints the page uses. Do not expose it to the
  public internet without adding authentication.
