#!/usr/bin/env python3
"""tools/mk-ca-bundle.py OLD.pem NEW_SYSTEM.pem OUT.pem — merge the host's current Mozilla-derived CA store with
still-valid legacy roots from the previous HobbyOS bundle (kept for cross-signed chains such as
example.com's 'SSL.com TLS ECC Root CA 2022' <- 'AAA Certificate Services'). Expired certs are dropped."""
import re, subprocess, sys, datetime
def certs(p):
    return re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", open(p).read(), re.S)
def info(pem):
    o = subprocess.run(["openssl","x509","-noout","-subject","-enddate","-fingerprint","-sha256","-nameopt","RFC2253"],
                       input=pem, capture_output=True, text=True).stdout
    subj = re.search(r"subject=(.*)", o).group(1)
    end = datetime.datetime.strptime(re.search(r"notAfter=(.*)", o).group(1).strip(), "%b %d %H:%M:%S %Y %Z")
    fp = re.search(r"Fingerprint=(.*)", o).group(1).strip()
    return subj, end, fp
# Legacy roots kept on top of the current store: roots being SUNSET by root programs (age, not
# misbehaviour) that many servers still chain to.  Distrusted CAs (Symantec/VeriSign/GeoTrust/thawte,
# Entrust, Camerfirma, ...) are deliberately NOT carried over.
KEEP = ("CN=AAA Certificate Services,", "CN=DigiCert Global Root CA,", "CN=DigiCert High Assurance EV Root CA,",
        "CN=DigiCert Assured ID Root CA,", "CN=GlobalSign Root CA,")
now = datetime.datetime.now(datetime.UTC).replace(tzinfo=None); out = []; seen = set()
for src, tag in ((sys.argv[2], "system"), (sys.argv[1], "legacy")):
    for pem in certs(src):
        subj, end, fp = info(pem)
        if fp in seen or end < now: continue
        if tag == "legacy" and not subj.startswith(KEEP): continue
        seen.add(fp); out.append(f"# {subj}\n# source: {tag}; notAfter {end:%Y-%m-%d}; SHA256 {fp}\n{pem}\n")
hdr = (f"# HobbyOS CA bundle (/CERTS/CA.PEM) generated {now:%Y-%m-%d} by tools/mk-ca-bundle.py:\n"
       f"# host Mozilla-derived ca-certificates store + a few still-valid sunsetting legacy roots (see KEEP).\n"
       f"# {len(out)} certificates.\n\n")
open(sys.argv[3], "w").write(hdr + "\n".join(out)); print(len(out), "certs ->", sys.argv[3])
