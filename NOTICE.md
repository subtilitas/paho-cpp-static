# Provenance and third-party notices

## This project

`paho-cpp-static` is licensed under the MIT licence (see `LICENSE`).

The repository name references Eclipse Paho because this project was started as
an answer to the question "what would Paho MQTT C look like if it could not use
a heap?", and it was developed alongside a Paho MQTT C checkout used for
reference and comparison. It is worth being precise about what that does and
does not mean:

- **No Eclipse Paho source code was copied into this repository.** The client is
  an independent implementation written against the OASIS *MQTT Version 3.1.1*
  specification, which is a published open standard. Packet layouts, the
  variable byte integer encoding, the QoS handshakes and the topic wildcard
  rules all come from that specification.
- The architecture is materially different from Paho's: no dynamic allocation,
  no linked lists or trees, no threads, no compiled-in socket or TLS layer, a
  different public API, and C++17 rather than C.
- A handful of source comments refer to Paho by name where this implementation
  deliberately behaves differently (for example, rejecting non-minimal variable
  byte integer encodings that Paho accepts). Those are comparative remarks, not
  derived text.

Eclipse Paho MQTT C is itself distributed under EPL-2.0 and EDL-1.0. Those terms
do not attach to this repository, because none of its code is present here.

If the licensing position matters for a commercial deployment, have it reviewed
by someone qualified rather than relying on this file. It is an accurate
description of how the code was produced, not legal advice.

## Dependencies

### Embedded Template Library (ETL)

This project depends on the [Embedded Template Library](https://www.etlcpp.com/)
for its fixed-capacity containers and byte stream serialization.

ETL is copyright John Wellbelove and contributors, and is distributed under the
MIT licence. It is consumed as an external dependency — either fetched by CMake
or pointed at with `-DMQTT_ETL_DIR=...` — and no ETL source is vendored into
this repository.

### MQTT specification

MQTT Version 3.1.1 is an OASIS Standard, published under the OASIS IPR Policy on
a royalty-free basis:
<http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html>

MQTT is a trademark of the OASIS open standards consortium. This project is an
independent implementation and is not endorsed by or affiliated with OASIS or
with the Eclipse Foundation.
