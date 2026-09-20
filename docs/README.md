# libftp Documentation

libftp is the transport-agnostic FTP (RFC 959) control-line engine
(`include/libftp.h`) at the heart of this repository's real FTP server,
[ftpd](../tools/ftpd) - see that module's own docs for the server itself
(command handling, PASV/PORT data transfer, running it as a dmsystem
service).

## Contents

- **[api-reference.md](api-reference.md)** - Complete API documentation for this engine
- **[../tools/ftpd/docs/service.md](../tools/ftpd/docs/service.md)** - Running ftpd as a dmsystem service
- **[../tools/ftpd/README.md](../tools/ftpd/README.md)** - ftpd's own README (supported commands, configuration, known limitations)

## Quick Reference

```c
#include "libftp.h"
```

View documentation using `dmf-man`:

```bash
dmf-man libftp          # Main documentation
dmf-man libftp api      # API reference
```
