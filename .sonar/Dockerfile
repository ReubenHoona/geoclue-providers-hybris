# Image for .sonar/coverage.sh: ~/stacks/sonarqube/scan.sh builds it (context: this .sonar/ directory only) and runs
# the script in it as the scanning user. A native x86_64 build of tests/test_qmiloc.c (the QMI LOC client against a
# fake LOC service behind stubbed socket calls) with gcc --coverage and gcovr for the Cobertura report; the watch
# builds the provider with the Yocto SDK, which is not available here. linux-libc-dev brings <linux/qrtr.h>.
# The base image is pinned; nothing is downloaded when the coverage runs.
FROM debian:trixie-20261005-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev linux-libc-dev make gcovr \
    && rm -rf /var/lib/apt/lists/*
