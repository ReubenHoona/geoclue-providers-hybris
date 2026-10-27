#!/bin/sh
# Coverage of qmi/qmiloc.c (QMI LOC over QRTR: framing, TLV parsing, XTRA injection, service tracking) from the
# host test tests/test_qmiloc.c, which includes qmiloc.c with sendto/recvfrom/poll/socket stubbed by a fake LOC
# service. qmilocationbackend.cpp (the Qt/GeoClue side) is not covered: it needs the provider's D-Bus stack.
# Run by ~/stacks/sonarqube/scan.sh in the image from .sonar/Dockerfile, cwd = the repository root.
# Local run: sh .sonar/coverage.sh (needs gcc and gcovr); the report is .sonar-reports/cobertura.xml.
set -eu
out=.sonar-reports
build=$out/build
rm -rf "$build"
mkdir -p "$build"
gcc -std=gnu11 -O0 -g --coverage -Wall -Wextra -Iqmi -o "$build/test_qmiloc" tests/test_qmiloc.c -lm
(cd "$build" && ./test_qmiloc)
gcovr --root . --filter 'qmi/qmiloc\.[ch]$' --object-directory "$build" \
  --cobertura "$out/cobertura.xml" --txt "$out/coverage.txt" "$build"
cat "$out/coverage.txt"
