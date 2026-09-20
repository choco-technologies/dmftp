# dmftp Documentation

dmftp is an FTP (RFC 959) server for DMOD boards: a transport-agnostic
control-line engine (`include/dmftp.h`) plus a dmtcp/filesystem-backed
server built on top of it, started automatically at boot as a dmsystem
service.

## Contents

- **[api-reference.md](api-reference.md)** - Complete API documentation
- **[service.md](service.md)** - Running dmftp as a dmsystem service

## Quick Reference

```c
#include "dmftp.h"
```

View documentation using `dmf-man`:

```bash
dmf-man dmftp          # Main documentation
dmf-man dmftp api      # API reference
```
