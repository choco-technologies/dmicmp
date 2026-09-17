# ping Documentation

Welcome to the ping module documentation.

## Contents

- **[ping.md](ping.md)** - Usage, exit codes, and how this module is tested

## Quick Reference

```
ping <address>       Send 4 ICMPv4 Echo Requests to <address>
ping --help | -h     Show this help

Options:
  -c <count>         Number of Echo Requests to send (default: 4)
  -i <interval_ms>   Delay between requests, in milliseconds (default: 1000)
  -W <timeout_ms>    Time to wait for a reply before it's lost, in milliseconds (default: 1000)
  -s <size>          Number of payload bytes to send, 0-1024 (default: 32)
```

View documentation using `dmf-man`:

```bash
dmf-man ping       # Main documentation
dmf-man ping ping  # ping.md
```
