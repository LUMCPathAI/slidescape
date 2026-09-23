# Public TLS test fixtures

These certificates and private keys are intentionally public, test-only data.
Never use them as deployment credentials or install this CA in an OS trust store.
The CA private key is discarded during generation.

The RSA-2048 CA has a SHA-1 self-signature to test legacy trust-anchor parsing.
The RSA-2048 and ECDSA P-256 server certificates have SHA-256 signatures, the
`localhost` DNS name, and validity from 2020 through 2040. `expired` ended in
2021; `sha1` has a deliberately unacceptable SHA-1 server signature.

Run `python3 tests/fixtures/tls/generate.py` with OpenSSL installed to regenerate
all fixtures together. This creates new random keys; byte-identical outputs are
not expected. Generation is not part of the build or test run.
