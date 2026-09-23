#!/usr/bin/env python3
"""Regenerate PUBLIC TEST ONLY TLS fixtures using OpenSSL (not needed to build/test)."""
from pathlib import Path
import subprocess
import tempfile

out = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="slidescape-tls-fixtures-") as directory:
    work = Path(directory)

    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=work, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    (work / "index").touch()
    (work / "serial").write_text("1000\n")
    (work / "ca.cnf").write_text("""[ca]
default_ca = local
[local]
database = index
serial = serial
new_certs_dir = .
private_key = ca.key
certificate = ca.pem
default_md = sha256
policy = names
[names]
commonName = supplied
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
""")
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
            "-out", "ca.csr", "-subj", "/CN=Slidescape TEST ONLY CA")
    # Real OS trust stores contain SHA-1 self-signed roots. Trusting such a root
    # must not enable SHA-1 signatures on server/intermediate certificates.
    openssl("ca", "-batch", "-selfsign", "-config", "ca.cnf", "-extensions", "root",
            "-md", "sha1", "-in", "ca.csr", "-out", "ca.pem", "-notext",
            "-startdate", "20200101000000Z", "-enddate", "20400101000000Z")
    for kind in ("rsa", "ecdsa", "expired", "sha1"):
        key_args = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"] if kind == "ecdsa" else ["-newkey", "rsa:2048"]
        openssl("req", "-new", *key_args, "-nodes", "-keyout", kind + ".key",
                "-out", kind + ".csr", "-subj", "/CN=Slidescape TEST ONLY " + kind)
        openssl("ca", "-batch", "-config", "ca.cnf", "-extensions", "leaf",
                "-md", "sha1" if kind == "sha1" else "sha256",
                "-in", kind + ".csr", "-out", kind + ".pem", "-notext",
                "-startdate", "20200101000000Z", "-enddate",
                "20210101000000Z" if kind == "expired" else "20400101000000Z")
        for suffix in (".key", ".pem"):
            (out / (kind + suffix)).write_bytes((work / (kind + suffix)).read_bytes())
    (out / "ca.pem").write_bytes((work / "ca.pem").read_bytes())
