# tools/roots/ -- provenance of the built-in TLS trust store

This directory holds the PEM root-CA certificates `tools/genroots.py`
extracts into `c/crypto/trust/roots_bundle.inc`, the trust anchors compiled
into the kernel. Until this file was written the directory had no README and
no recorded source (`THIRD_PARTY.md:159-161`: *"The exact source bundle
revision, download URL, and SHA-256 were not recorded at import time, so the
repository cannot currently reproduce the external bundle provenance
byte-for-byte from a named upstream snapshot."*). This file, and the two TSV
ledgers beside it, are that record.

Everything below is measured, on this machine, on the date given. **The tree
was NOT changed by this audit**: `tools/roots/*.pem` (130 files) and
`c/crypto/trust/roots_bundle.inc` are exactly what they were before it. What
was added is this README, `certdata-fingerprints.txt`,
`legacy-exceptions.txt`, and `check_provenance.py` -- a record and a check,
not a swap.

## 1. What is there today

130 PEM files, no prior README, no prior recorded source.
`THIRD_PARTY.md:152-154` records how they arrived: *"Git history records that
commit `98d59fc` imported certificates from the host's `/etc/ssl/cert.pem`,
described there as an authentic Mozilla/NSS-derived store, then deduplicated
the set by SubjectPublicKeyInfo."* -- i.e. a macOS trust-store snapshot from
2026-06-08, not a direct copy of any one Mozilla `certdata.txt`.

For each of the 130, the SHA-256 fingerprint is computed over the **DER**
encoding of the certificate (`openssl x509 -in <file> -noout -fingerprint
-sha256`, which fingerprints the decoded certificate object, not the PEM
text or the file bytes), alongside its subject
(`openssl x509 -in <file> -noout -subject -nameopt RFC2253`). No duplicate
fingerprints exist among the 130.

## 2. What it should be: Mozilla's certdata.txt

Mozilla's `certdata.txt` is the upstream every OS/browser root bundle is
either a copy of or a derivative of, including Apple's (which is where this
tree's own 130 ultimately trace back to, per THIRD_PARTY.md above).

- **Source URL fetched**: `https://hg.mozilla.org/mozilla-central/raw-file/tip/security/nss/lib/ckfw/builtins/certdata.txt`
  (redirected by the server, HTTP 302, to
  `https://hg-edge.mozilla.org/mozilla-central/raw-file/tip/security/nss/lib/ckfw/builtins/certdata.txt`
  -- `hg-edge` is Mozilla's own edge mirror of `hg.mozilla.org`, not a
  third-party fork; `curl -L` followed it automatically).
- **Fetched**: 2026-08-21, `HTTP 200`, 1,375,098 bytes.
- **SHA-256 of the fetched file**:
  `81b7f2576333a2e360e673f912d7b0b7a765d836c731003e348a46cac5d37198`
- **License notice, quoted from the file itself** (`certdata.txt:2-4`):
  ```
  # This Source Code Form is subject to the terms of the Mozilla Public
  # License, v. 2.0. If a copy of the MPL was not distributed with this
  # file, You can obtain one at http://mozilla.org/MPL/2.0/.
  ```
  This is the **MPL-2.0 notice** `THIRD_PARTY.md:157` already names for this
  component ("Mozilla-derived PEM CA bundles are distributed under MPL 2.0")
  -- now backed by the exact file and lines it comes from, not a recollection.
  `certdata.txt` carries **no comments anywhere in the file** explaining why
  any individual root's trust was changed or removed (checked:
  `grep -in "bug\|distrust\|removed\|deprecat" certdata.txt` inside comment
  lines returns nothing) -- so no in-file citation exists for *why* a given
  root moved, only *that* it did.

This is the **one network fetch** this audit made, per the task's
instruction. Everything else below is computed from the fetched file and
from what was already in the repository.

### Extraction

`certdata.txt` is NSS's own PKCS#11 object-definition format: a
`CKO_CERTIFICATE` object (DER cert in `CKA_VALUE`) paired by `CKA_LABEL`
with a `CKO_NSS_TRUST` object carrying `CKA_TRUST_SERVER_AUTH`. Parsed with a
purpose-built script (not committed -- this ledger is its output, which is
what needs to survive):

- 172 `CKO_CERTIFICATE` objects, 172 `CKO_NSS_TRUST` objects, one-to-one by
  label (no orphans, no duplicate labels, no multi-trust-record labels --
  checked).
- `CKA_TRUST_SERVER_AUTH`: **121** `CKT_NSS_TRUSTED_DELEGATOR` (trusted as a
  TLS server-auth anchor), **51** `CKT_NSS_MUST_VERIFY_TRUST` (present in the
  file, not a TLS anchor). Cross-checked independently with plain grep:
  `grep "^CKA_TRUST_SERVER_AUTH" certdata.txt | sort | uniq -c` reports the
  same 121/51 split.

`certdata-fingerprints.txt` (committed beside this file) records all 172,
sorted by label, as `sha256_of_der<TAB>CKA_TRUST_SERVER_AUTH<TAB>label`.

### The Mozilla-derived candidate bundle

The 121 `CKT_NSS_TRUSTED_DELEGATOR` certificates were written out as
individual PEMs (named by a filesystem-safe slug of their `CKA_LABEL`) and
run through **this repository's own, unmodified** `tools/genroots.py`
(WSL/Linux `python3`, matching the toolchain this tree is built with, to
avoid the CRLF a native-Windows Python `open(..., "w")` would introduce):

```
$ python3 tools/genroots.py <121-pem-dir> <scratch-output>.inc
wrote <scratch-output>.inc (121 roots; 0 skipped)
```

**genroots.py's key-type skip removes 0 of the 121** -- every Mozilla
server-auth-trusted root today is RSA or EC P-256/P-384, nothing the kernel's
verifier cannot check. For comparison, running the same unmodified
`genroots.py` against the CURRENT `tools/roots/` (130 files) also skips 0,
and reproduces `c/crypto/trust/roots_bundle.inc` **byte-for-byte**
(`diff` clean) when run under the same WSL/Linux Python the Makefile uses --
confirming the committed generator and the committed output still agree.

## 3. The diff, by fingerprint (not by name)

130 of ours vs. 121 Mozilla-trusts-today, joined on the SHA-256-of-DER
fingerprint computed in step 1. Arithmetic: 130 = 60 + 70, 121 = 51 + 70.

### 3a. Roots we hold that Mozilla no longer trusts for server auth -- 60

This is the security-relevant list: a root we have been trusting since the
2026-06-08 import that Mozilla's current file does not trust for TLS, for
one of two different reasons that matter differently:

**A1. Present in certdata.txt today, but marked `CKT_NSS_MUST_VERIFY_TRUST`
(18)** -- Mozilla still ships and tracks these; it has deliberately withdrawn
their server-auth trust bit.

| tools/roots file | certdata.txt label | CKA_TRUST_SERVER_AUTH |
|---|---|---|
| `aaa_certificate_services.pem` | Comodo AAA Services root | `CKT_NSS_MUST_VERIFY_TRUST` |
| `atos_trustedroot_2011.pem` | Atos TrustedRoot 2011 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `certigna.pem` | Certigna | `CKT_NSS_MUST_VERIFY_TRUST` |
| `comodo_certification_authority.pem` | COMODO Certification Authority | `CKT_NSS_MUST_VERIFY_TRUST` |
| `digicert_assured_id_root_ca.pem` | DigiCert Assured ID Root CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `digicert_global_ca.pem` | DigiCert Global Root CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `digicert_high_assurance_ev_root_ca.pem` | DigiCert High Assurance EV Root CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `entrust_net_certification_authority_2048.pem` | Entrust.net Premium 2048 Secure Server CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `entrust_root_certification_authority_ec1.pem` | Entrust Root Certification Authority - EC1 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `entrust_root_certification_authority_g2.pem` | Entrust Root Certification Authority - G2 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `entrust_root_certification_authority_g4.pem` | Entrust Root Certification Authority - G4 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `globalsign_r1.pem` | GlobalSign Root CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `quovadis_root_ca_2.pem` | QuoVadis Root CA 2 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `quovadis_root_ca_3.pem` | QuoVadis Root CA 3 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `staat_der_nederlanden_root_ca_g3.pem` | Staat der Nederlanden Root CA - G3 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `starfield_technologies.pem` | Starfield Class 2 CA | `CKT_NSS_MUST_VERIFY_TRUST` |
| `swisssign_gold_ca_g2.pem` | SwissSign Gold CA - G2 | `CKT_NSS_MUST_VERIFY_TRUST` |
| `the_go_daddy_group.pem` | Go Daddy Class 2 CA | `CKT_NSS_MUST_VERIFY_TRUST` |

**A2. Not present in certdata.txt at all today (42)** -- Mozilla's shipped
file no longer carries this exact certificate object in any form. These are
recorded in `legacy-exceptions.txt` (see §5) so a provenance check can still
account for them without re-trusting them by default.

| tools/roots file | subject |
|---|---|
| `affirmtrust_commercial.pem` | CN=AffirmTrust Commercial,O=AffirmTrust,C=US |
| `affirmtrust_networking.pem` | CN=AffirmTrust Networking,O=AffirmTrust,C=US |
| `affirmtrust_premium.pem` | CN=AffirmTrust Premium,O=AffirmTrust,C=US |
| `affirmtrust_premium_ecc.pem` | CN=AffirmTrust Premium ECC,O=AffirmTrust,C=US |
| `autoridad_de_certificacion_firmaprofesional_cif_.pem` | CN=Autoridad de Certificacion Firmaprofesional CIF A62634068,C=ES |
| `baltimore_cybertrust_root.pem` | CN=Baltimore CyberTrust Root,OU=CyberTrust,O=Baltimore,C=IE |
| `certsign.pem` | OU=certSIGN ROOT CA,O=certSIGN,C=RO |
| `chambers_of_commerce_root_2008.pem` | CN=Chambers of Commerce Root - 2008,O=AC Camerfirma S.A.,serialNumber=A82743287,L=Madrid (see current address at www.camerfirma.com/address),C=EU |
| `chunghwa_telecom_co.pem` | OU=ePKI Root Certification Authority,O=Chunghwa Telecom Co.\, Ltd.,C=TW |
| `cybertrust_global_root.pem` | CN=Cybertrust Global Root,O=Cybertrust\, Inc |
| `e_tugra_certification_authority.pem` | CN=E-Tugra Certification Authority,OU=E-Tugra Sertifikasyon Merkezi,O=E-Tu\C4\9Fra EBG Bili\C5\9Fim Teknolojileri ve Hizmetleri A.\C5\9E.,L=Ankara,C=TR |
| `ec_acc.pem` | CN=EC-ACC,OU=Jerarquia Entitats de Certificacio Catalanes,OU=Vegeu https://www.catcert.net/verarrel (c)03,OU=Serveis Publics de Certificacio,O=Agencia Catalana de Certificacio (NIF Q-0801176-I),C=ES |
| `entrust_root_certification_authority.pem` | CN=Entrust Root Certification Authority,OU=(c) 2006 Entrust\, Inc.,OU=www.entrust.net/CPS is incorporated by reference,O=Entrust\, Inc.,C=US |
| `geotrust_global_ca.pem` | CN=GeoTrust Global CA,O=GeoTrust Inc.,C=US |
| `geotrust_primary_certification_authority_g2.pem` | CN=GeoTrust Primary Certification Authority - G2,OU=(c) 2007 GeoTrust Inc. - For authorized use only,O=GeoTrust Inc.,C=US |
| `global_chambersign_root_2008.pem` | CN=Global Chambersign Root - 2008,O=AC Camerfirma S.A.,serialNumber=A82743287,L=Madrid (see current address at www.camerfirma.com/address),C=EU |
| `globalsign.pem` | CN=GlobalSign,O=GlobalSign,OU=GlobalSign ECC Root CA - R4 |
| `globalsign_2.pem` | CN=GlobalSign,O=GlobalSign,OU=GlobalSign Root CA - R2 |
| `gts_root_r2.pem` | CN=GTS Root R2,O=Google Trust Services LLC,C=US |
| `gts_root_r3.pem` | CN=GTS Root R3,O=Google Trust Services LLC,C=US |
| `hellenic_academic_and_research_institutions_root_1.pem` | CN=Hellenic Academic and Research Institutions RootCA 2011,O=Hellenic Academic and Research Institutions Cert. Authority,C=GR |
| `hongkong_post_root_ca_1.pem` | CN=Hongkong Post Root CA 1,O=Hongkong Post,C=HK |
| `network_solutions_certificate_authority.pem` | CN=Network Solutions Certificate Authority,O=Network Solutions L.L.C.,C=US |
| `quovadis_root_certification_authority.pem` | CN=QuoVadis Root Certification Authority,OU=Root Certification Authority,O=QuoVadis Limited,C=BM |
| `secom_trust_net.pem` | OU=Security Communication RootCA1,O=SECOM Trust.net,C=JP |
| `secure_global_ca.pem` | CN=Secure Global CA,O=SecureTrust Corporation,C=US |
| `securesign_rootca11.pem` | CN=SecureSign RootCA11,O=Japan Certification Services\, Inc.,C=JP |
| `securetrust_ca.pem` | CN=SecureTrust CA,O=SecureTrust Corporation,C=US |
| `sonera_class2_ca.pem` | CN=Sonera Class2 CA,O=Sonera,C=FI |
| `sslcom_ecc_2022.pem` | CN=SSL.com TLS ECC Root CA 2022,O=SSL Corporation,C=US |
| `staat_der_nederlanden_ev_root_ca.pem` | CN=Staat der Nederlanden EV Root CA,O=Staat der Nederlanden,C=NL |
| `swisssign_silver_ca_g2.pem` | CN=SwissSign Silver CA - G2,O=SwissSign AG,C=CH |
| `teliasonera_root_ca_v1.pem` | CN=TeliaSonera Root CA v1,O=TeliaSonera |
| `trustcor_eca_1.pem` | CN=TrustCor ECA-1,OU=TrustCor Certificate Authority,O=TrustCor Systems S. de R.L.,L=Panama City,ST=Panama,C=PA |
| `trustcor_rootcert_ca_1.pem` | CN=TrustCor RootCert CA-1,OU=TrustCor Certificate Authority,O=TrustCor Systems S. de R.L.,L=Panama City,ST=Panama,C=PA |
| `trustcor_rootcert_ca_2.pem` | CN=TrustCor RootCert CA-2,OU=TrustCor Certificate Authority,O=TrustCor Systems S. de R.L.,L=Panama City,ST=Panama,C=PA |
| `trustis_limited.pem` | OU=Trustis FPS Root CA,O=Trustis Limited,C=GB |
| `trustwave_global_certification_authority.pem` | CN=Trustwave Global Certification Authority,O=Trustwave Holdings\, Inc.,L=Chicago,ST=Illinois,C=US |
| `trustwave_global_ecc_p256_certification_authorit.pem` | CN=Trustwave Global ECC P256 Certification Authority,O=Trustwave Holdings\, Inc.,L=Chicago,ST=Illinois,C=US |
| `trustwave_global_ecc_p384_certification_authorit.pem` | CN=Trustwave Global ECC P384 Certification Authority,O=Trustwave Holdings\, Inc.,L=Chicago,ST=Illinois,C=US |
| `verisign_universal_root_certification_authority.pem` | CN=VeriSign Universal Root Certification Authority,OU=(c) 2008 VeriSign\, Inc. - For authorized use only,OU=VeriSign Trust Network,O=VeriSign\, Inc.,C=US |
| `xramp_global_certification_authority.pem` | CN=XRamp Global Certification Authority,O=XRamp Security Services Inc,OU=www.xrampsecurity.com,C=US |

**Named, well-documented cases inside A1/A2** (general knowledge, not
sourced from `certdata.txt`'s comments -- confirmed above that the file has
none -- and not re-verified against a second primary source in this session,
since the one authorized fetch was already spent on `certdata.txt` itself;
stated with that caveat, not as a verified-here fact):

- **The four Entrust roots** (`entrust_root_certification_authority_ec1.pem`,
  `_g2.pem`, `_g4.pem`, and the older `entrust_net_certification_authority_2048.pem`)
  are all `CKT_NSS_MUST_VERIFY_TRUST` with **no** distrust-after date (i.e. a
  full, ungrandfathered removal, not a phase-out) -- consistent with the
  publicly reported 2024 Entrust distrust action by Mozilla and other root
  programs following a pattern of compliance failures. All four read this way
  in the fetched file; that pattern, not a bug number, is what is being
  reported here.
- **TrustCor** (`trustcor_eca_1.pem`, `trustcor_rootcert_ca_1.pem`,
  `trustcor_rootcert_ca_2.pem`, all in A2 -- not in `certdata.txt` at all) is
  the other widely reported case: multiple root programs removed TrustCor
  around 2022 following reporting that linked the organization to
  surveillance-ware operations.
- The rest of A1 (Comodo/COMODO, the DigiCert 1st-generation roots, GlobalSign
  Root CA R1, Go Daddy/Starfield Class 2, QuoVadis 2/3, SwissSign Gold G2,
  Certigna, Atos TrustedRoot 2011, Staat der Nederlanden G3) read as routine
  root-program hygiene -- 2016-vintage or 1024/2048-bit-era anchors whose
  issuers have newer-generation replacements that ARE in the "both" list
  below (e.g. `globalsign_r3.pem` / GlobalSign Root CA - R3 is trusted while
  `globalsign_r1.pem` / GlobalSign Root CA is not) -- but no in-file citation
  supports that reading beyond the pattern itself, and none is claimed.

### 3b. Roots Mozilla trusts today that we lack -- 51

| certdata.txt label | subject |
|---|---|
| AC RAIZ FNMT-RCM SERVIDORES SEGUROS | CN=AC RAIZ FNMT-RCM SERVIDORES SEGUROS,organizationIdentifier=VATES-Q2826004J,OU=Ceres,O=FNMT-RCM,C=ES |
| ANF Secure Server Root CA | CN=ANF Secure Server Root CA,OU=ANF CA Raiz,O=ANF Autoridad de Certificacion,C=ES,serialNumber=G63287510 |
| Atos TrustedRoot Root CA ECC TLS 2021 | C=DE,O=Atos,CN=Atos TrustedRoot Root CA ECC TLS 2021 |
| Atos TrustedRoot Root CA RSA TLS 2021 | C=DE,O=Atos,CN=Atos TrustedRoot Root CA RSA TLS 2021 |
| Autoridad de Certificacion Firmaprofesional CIF A62634068 | CN=Autoridad de Certificacion Firmaprofesional CIF A62634068,C=ES |
| BJCA Global Root CA1 | CN=BJCA Global Root CA1,O=BEIJING CERTIFICATE AUTHORITY,C=CN |
| BJCA Global Root CA2 | CN=BJCA Global Root CA2,O=BEIJING CERTIFICATE AUTHORITY,C=CN |
| Certainly Root E1 | CN=Certainly Root E1,O=Certainly,C=US |
| Certainly Root R1 | CN=Certainly Root R1,O=Certainly,C=US |
| Certum EC-384 CA | CN=Certum EC-384 CA,OU=Certum Certification Authority,O=Asseco Data Systems S.A.,C=PL |
| Certum Trusted Network CA 2 | CN=Certum Trusted Network CA 2,OU=Certum Certification Authority,O=Unizeto Technologies S.A.,C=PL |
| Certum Trusted Root CA | CN=Certum Trusted Root CA,OU=Certum Certification Authority,O=Asseco Data Systems S.A.,C=PL |
| D-TRUST BR Root CA 1 2020 | CN=D-TRUST BR Root CA 1 2020,O=D-Trust GmbH,C=DE |
| D-TRUST BR Root CA 2 2023 | CN=D-TRUST BR Root CA 2 2023,O=D-Trust GmbH,C=DE |
| D-TRUST EV Root CA 1 2020 | CN=D-TRUST EV Root CA 1 2020,O=D-Trust GmbH,C=DE |
| D-TRUST EV Root CA 2 2023 | CN=D-TRUST EV Root CA 2 2023,O=D-Trust GmbH,C=DE |
| DigiCert TLS ECC P384 Root G5 | CN=DigiCert TLS ECC P384 Root G5,O=DigiCert\, Inc.,C=US |
| DigiCert TLS RSA4096 Root G5 | CN=DigiCert TLS RSA4096 Root G5,O=DigiCert\, Inc.,C=US |
| e-Szigno TLS Root CA 2023 | CN=e-Szigno TLS Root CA 2023,organizationIdentifier=VATHU-23584497,O=Microsec Ltd.,L=Budapest,C=HU |
| GlobalSign ECC Root CA - R4 | CN=GlobalSign,O=GlobalSign,OU=GlobalSign ECC Root CA - R4 |
| GlobalSign Root E46 | CN=GlobalSign Root E46,O=GlobalSign nv-sa,C=BE |
| GlobalSign Root R46 | CN=GlobalSign Root R46,O=GlobalSign nv-sa,C=BE |
| GTS Root R3 | CN=GTS Root R3,O=Google Trust Services LLC,C=US |
| HARICA TLS ECC Root CA 2021 | CN=HARICA TLS ECC Root CA 2021,O=Hellenic Academic and Research Institutions CA,C=GR |
| HARICA TLS RSA Root CA 2021 | CN=HARICA TLS RSA Root CA 2021,O=Hellenic Academic and Research Institutions CA,C=GR |
| HiPKI Root CA - G1 | CN=HiPKI Root CA - G1,O=Chunghwa Telecom Co.\, Ltd.,C=TW |
| OISTE Server Root ECC G1 | CN=OISTE Server Root ECC G1,O=OISTE Foundation,C=CH |
| OISTE Server Root RSA G1 | CN=OISTE Server Root RSA G1,O=OISTE Foundation,C=CH |
| SECOM TLS ECC Root CA 2024 | CN=SECOM TLS ECC Root CA 2024,O=SECOM Trust Systems Co.\, Ltd.,C=JP |
| SECOM TLS RSA Root CA 2024 | CN=SECOM TLS RSA Root CA 2024,O=SECOM Trust Systems Co.\, Ltd.,C=JP |
| Sectigo Public Server Authentication Root E46 | CN=Sectigo Public Server Authentication Root E46,O=Sectigo Limited,C=GB |
| Sectigo Public Server Authentication Root R46 | CN=Sectigo Public Server Authentication Root R46,O=Sectigo Limited,C=GB |
| SecureSign Root CA14 | CN=SecureSign Root CA14,O=Cybertrust Japan Co.\, Ltd.,C=JP |
| SecureSign Root CA15 | CN=SecureSign Root CA15,O=Cybertrust Japan Co.\, Ltd.,C=JP |
| Security Communication ECC RootCA1 | CN=Security Communication ECC RootCA1,O=SECOM Trust Systems CO.\,LTD.,C=JP |
| SSL.com TLS ECC Root CA 2022 | CN=SSL.com TLS ECC Root CA 2022,O=SSL Corporation,C=US |
| SSL.com TLS RSA Root CA 2022 | CN=SSL.com TLS RSA Root CA 2022,O=SSL Corporation,C=US |
| SwissSign RSA TLS Root CA 2022 - 1 | CN=SwissSign RSA TLS Root CA 2022 - 1,O=SwissSign AG,C=CH |
| Telekom Security TLS ECC Root 2020 | CN=Telekom Security TLS ECC Root 2020,O=Deutsche Telekom Security GmbH,C=DE |
| Telekom Security TLS RSA Root 2023 | CN=Telekom Security TLS RSA Root 2023,O=Deutsche Telekom Security GmbH,C=DE |
| Telia EC TLS Root CA v3 | CN=Telia EC TLS Root CA v3,O=Telia Company AB,C=SE |
| Telia Root CA v2 | CN=Telia Root CA v2,O=Telia Finland Oyj,C=FI |
| Telia RSA TLS Root CA v3 | CN=Telia RSA TLS Root CA v3,O=Telia Company AB,C=SE |
| TrustAsia Global Root CA G3 | CN=TrustAsia Global Root CA G3,O=TrustAsia Technologies\, Inc.,C=CN |
| TrustAsia Global Root CA G4 | CN=TrustAsia Global Root CA G4,O=TrustAsia Technologies\, Inc.,C=CN |
| TrustAsia TLS ECC Root CA | CN=TrustAsia TLS ECC Root CA,O=TrustAsia Technologies\, Inc.,C=CN |
| TrustAsia TLS RSA Root CA | CN=TrustAsia TLS RSA Root CA,O=TrustAsia Technologies\, Inc.,C=CN |
| TunTrust Root CA | CN=TunTrust Root CA,O=Agence Nationale de Certification Electronique,C=TN |
| TWCA CYBER Root CA | CN=TWCA CYBER Root CA,OU=Root CA,O=TAIWAN-CA,C=TW |
| vTrus ECC Root CA | CN=vTrus ECC Root CA,O=iTrusChina Co.\,Ltd.,C=CN |
| vTrus Root CA | CN=vTrus Root CA,O=iTrusChina Co.\,Ltd.,C=CN |

### 3c. Roots in agreement -- 70

| tools/roots file | Mozilla certdata.txt label |
|---|---|
| `accvraiz1.pem` | ACCVRAIZ1 |
| `actalis_authentication_root_ca.pem` | Actalis Authentication Root CA |
| `amazon_root_1.pem` | Amazon Root CA 1 |
| `amazon_root_3.pem` | Amazon Root CA 3 |
| `amazon_root_ca_2.pem` | Amazon Root CA 2 |
| `amazon_root_ca_4.pem` | Amazon Root CA 4 |
| `buypass_class_2_root_ca.pem` | Buypass Class 2 Root CA |
| `buypass_class_3_root_ca.pem` | Buypass Class 3 Root CA |
| `ca_disig_root_r2.pem` | CA Disig Root R2 |
| `certigna_root_ca.pem` | Certigna Root CA |
| `certsign_sa.pem` | certSIGN Root CA G2 |
| `certum_trusted_network_ca.pem` | Certum Trusted Network CA |
| `cfca_ev_root.pem` | CFCA EV ROOT |
| `comodo_ecc_certification_authority.pem` | COMODO ECC Certification Authority |
| `comodo_rsa_certification_authority.pem` | COMODO RSA Certification Authority |
| `d_trust_root_class_3_ca_2_ev_2009.pem` | D-TRUST Root Class 3 CA 2 EV 2009 |
| `digicert_assured_id_root_g2.pem` | DigiCert Assured ID Root G2 |
| `digicert_assured_id_root_g3.pem` | DigiCert Assured ID Root G3 |
| `digicert_global_g2.pem` | DigiCert Global Root G2 |
| `digicert_global_g3.pem` | DigiCert Global Root G3 |
| `digicert_trusted_root_g4.pem` | DigiCert Trusted Root G4 |
| `dtrust_root3_2009.pem` | D-TRUST Root Class 3 CA 2 2009 |
| `e_szigno_root_ca_2017.pem` | e-Szigno Root CA 2017 |
| `emsign_ecc_root_ca_c3.pem` | emSign ECC Root CA - C3 |
| `emsign_ecc_root_ca_g3.pem` | emSign ECC Root CA - G3 |
| `emsign_root_ca_c1.pem` | emSign Root CA - C1 |
| `emsign_root_ca_g1.pem` | emSign Root CA - G1 |
| `fnmt_rcm.pem` | AC RAIZ FNMT-RCM |
| `gdca_trustauth_r5_root.pem` | GDCA TrustAUTH R5 ROOT |
| `globalsign_1.pem` | GlobalSign ECC Root CA - R5 |
| `globalsign_3.pem` | GlobalSign Root CA - R6 |
| `globalsign_r3.pem` | GlobalSign Root CA - R3 |
| `go_daddy_root_certificate_authority_g2.pem` | Go Daddy Root Certificate Authority - G2 |
| `gts_r1.pem` | GTS Root R1 |
| `gts_r4.pem` | GTS Root R4 |
| `hellenic_academic_and_research_institutions_ecc_.pem` | Hellenic Academic and Research Institutions ECC RootCA 2015 |
| `hellenic_academic_and_research_institutions_root.pem` | Hellenic Academic and Research Institutions RootCA 2015 |
| `hongkong_post_root_ca_3.pem` | Hongkong Post Root CA 3 |
| `identrust_commercial_root_ca_1.pem` | IdenTrust Commercial Root CA 1 |
| `identrust_public_sector_root_ca_1.pem` | IdenTrust Public Sector Root CA 1 |
| `isrg_x1.pem` | ISRG Root X1 |
| `isrg_x2.pem` | ISRG Root X2 |
| `izenpe_com.pem` | Izenpe.com |
| `microsec_e_szigno_root_ca_2009.pem` | Microsec e-Szigno Root CA 2009 |
| `microsoft_ecc_root_certificate_authority_2017.pem` | Microsoft ECC Root Certificate Authority 2017 |
| `microsoft_rsa_root_certificate_authority_2017.pem` | Microsoft RSA Root Certificate Authority 2017 |
| `naver_global_root_certification_authority.pem` | NAVER Global Root Certification Authority |
| `netlock_arany_class_gold_f_c5_91tan_c3_bas_c3_ad.pem` | NetLock Arany (Class Gold) Főtanúsítvány |
| `oiste_wisekey_global_root_gb_ca.pem` | OISTE WISeKey Global Root GB CA |
| `oiste_wisekey_global_root_gc_ca.pem` | OISTE WISeKey Global Root GC CA |
| `quovadis_root_ca_1_g3.pem` | QuoVadis Root CA 1 G3 |
| `quovadis_root_ca_2_g3.pem` | QuoVadis Root CA 2 G3 |
| `quovadis_root_ca_3_g3.pem` | QuoVadis Root CA 3 G3 |
| `secom_trust_systems_co.pem` | Security Communication RootCA2 |
| `ssl_com_ev_root_certification_authority_ecc.pem` | SSL.com EV Root Certification Authority ECC |
| `ssl_com_ev_root_certification_authority_rsa_r2.pem` | SSL.com EV Root Certification Authority RSA R2 |
| `ssl_com_root_certification_authority_ecc.pem` | SSL.com Root Certification Authority ECC |
| `ssl_com_root_certification_authority_rsa.pem` | SSL.com Root Certification Authority RSA |
| `starfield_root_certificate_authority_g2.pem` | Starfield Root Certificate Authority - G2 |
| `starfield_services_root_certificate_authority_g2.pem` | Starfield Services Root Certificate Authority - G2 |
| `szafir_root_ca2.pem` | SZAFIR ROOT CA2 |
| `t_telesec_globalroot_class_2.pem` | T-TeleSec GlobalRoot Class 2 |
| `t_telesec_globalroot_class_3.pem` | T-TeleSec GlobalRoot Class 3 |
| `tubitak_kamu_sm_ssl_kok_sertifikasi_surum_1.pem` | TUBITAK Kamu SM SSL Kok Sertifikasi - Surum 1 |
| `twca_global_root_ca.pem` | TWCA Global Root CA |
| `twca_root_certification_authority.pem` | TWCA Root Certification Authority |
| `uca_extended_validation_root.pem` | UCA Extended Validation Root |
| `uca_global_g2_root.pem` | UCA Global G2 Root |
| `usertrust_ecc.pem` | USERTrust ECC Certification Authority |
| `usertrust_rsa.pem` | USERTrust RSA Certification Authority |

### 3d. A refinement the raw fingerprint diff hides: 4 same-key pairs

The task asked for the diff by fingerprint, not by name, and §3a/3b/3c are
exactly that. But `genroots.py` itself extracts **only the
SubjectPublicKeyInfo** (an RSA `n,e` or an EC point) into
`roots_bundle.inc` -- *"the kernel trusts a root by verifying that the top of
a presented chain is signed by (or identical to) one of these keys"*
(`tools/genroots.py:6-7`) -- so two DIFFERENT certificate objects wrapping
the SAME key are the same trust anchor as far as this codebase's verifier is
concerned, even though they have different SHA-256-of-DER fingerprints (a
different serial number, extensions, or re-signature changes the DER without
changing the key). Comparing the SPKI (not the whole DER) of every A-list
entry against every B-list entry found **4 such pairs**:

| ours (A-list) | Mozilla's current object (B-list) | what differs |
|---|---|---|
| `gts_root_r3.pem` | GTS Root R3 | different serial + KeyUsage (ours lacks `Digital Signature`; Mozilla's current object has it) + signature -- same EC P-384 key |
| `sslcom_ecc_2022.pem` | SSL.com TLS ECC Root CA 2022 | ours is **cross-signed by AAA Certificate Services** (issuer/subject differ, `notBefore` 2025 vs. self-signed 2022), Mozilla's is the self-signed original -- same EC P-384 key |
| `globalsign.pem` | GlobalSign ECC Root CA - R4 | different certificate object, same EC key |
| `autoridad_de_certificacion_firmaprofesional_cif_.pem` | Autoridad de Certificacion Firmaprofesional CIF A62634068 | different certificate object, same RSA key |

So the true count of KEYS (not certificate objects) we hold that Mozilla's
current set does not is **56**, not 60, and the true coverage gap in KEYS is
**47**, not 51. `gts_root_r2.pem`, by contrast, does NOT share a key with
Mozilla's current "GTS Root R2" -- that one is a genuine gap, not a
re-signing. This refinement does not change §3a/3b/3c, which answer the
question exactly as asked (by fingerprint); it is reported because it is the
more accurate answer to "would swapping the bundle actually change what gets
trusted".

## 4. Regenerating `roots_bundle.inc` from the Mozilla-derived set: what was tried, what passed, what could not be run

**The tree was left unchanged; this section is a dry run, reported in full
per the task's instruction not to paper over a break.**

1. `tools/genroots.py` (unmodified) against the 121-PEM Mozilla-derived set
   produced a candidate `roots_bundle.inc` (121 roots, 0 skipped, confirmed
   §2).
2. That candidate was swapped into `c/crypto/trust/roots_bundle.inc` in the
   working tree, and:
   - `make test-crypto-bench-gate` (compiles `c/crypto/trust/roots.c` --
     i.e. the REAL production trust-store code, not a throwaway -- together
     with the candidate bundle into a host binary and runs it): **PASS**,
     both before (`rsa2048 56.1us / x25519 27.6us = 2.03x`, original bundle)
     and after (`56.7us / 27.4us = 2.07x`, candidate bundle).
   - `make test-tls` (`test-tls-interop test-tls-psk test-x509-fuzz
     test-crypto-diff`) with the candidate bundle linked: **PASS**, exit 0,
     140,214/140,214 crypto-diff cases passing, all TLS-interop and PSK
     cases passing.
   - The bundle was then **restored** to the original 130-root file
     (`diff` clean against a backup taken before the swap; `git status`
     clean throughout).
3. **What this did NOT prove, and could not, without exceeding the one
   authorized network fetch**: whether a REAL captured chain for the 5-8
   sites CLAUDE.md's M12.5 section names (example/google/github/wikipedia/
   kernel.org/bsi.bund.de/sectigo/D-TRUST) still verifies against the
   candidate bundle. That verification, when it was originally done, was not
   left behind as a repeatable, network-free repository test:
   - `tests/unit/run-tls-interop.sh:11-14` states plainly, of its own trust
     model: *"we mint a throwaway CA, generate a bundle from it with
     tools/genroots.py, and link that in place of the real 130-root store.
     The chain verification under test is therefore the production one --
     there is no 'trust anything' switch anywhere in this file or in
     tls.c."* -- i.e. this suite (and `run-tls-matrix.sh`,
     `run-tls13-certverify-bypass-probe.sh`, which do the same substitution)
     deliberately does NOT exercise the real committed
     `c/crypto/trust/roots_bundle.inc` at all.
   - The only test-tree callers of `x509_verify_chain` are `tls.c` itself,
     `tls_server_test.c`, and the throwaway-CA interop scripts above; no
     host-side, network-free harness in this tree feeds a captured
     real-world chain through the compiled `logit_roots[]`.
   - The actual real-chain verification the CLAUDE.md milestone note
     describes runs at the QEMU-boot level, with live outbound HTTPS to real
     Internet hosts (`tests/boot/run-https-smoke.sh:93`:
     `URLS=(https://cloudflare.com/ https://zh.wikipedia.org/
     https://www.bsi.bund.de/...)`; `tests/boot/run-h2-smoke.sh:52` similarly).
     Running these would mean additional live connections to external sites
     beyond the one fetch this task authorized, so **they were not run**.
4. **What can be said without a live handshake**, from the fingerprint- and
   SPKI-level data already gathered in §3: every root CLAUDE.md's M12.5
   section names by name as a real-chain anchor is present, UNCHANGED, in
   the Mozilla-derived candidate --
   `gts_r1.pem` (GTS Root R1, used by google.com/github.com-class chains),
   `isrg_x1.pem` (ISRG Root X1, Let's Encrypt, used by kernel.org-class
   chains), `dtrust_root3_2009.pem` (D-TRUST Root Class 3 CA 2 2009, the
   SHA-512 anchor CLAUDE.md names explicitly for bsi.bund.de),
   `usertrust_rsa.pem` (USERTrust RSA Certification Authority, the sectigo
   chain's anchor), and the DigiCert Global Root G2/G3/Trusted Root G4 family
   (example.com/wikipedia.org-class chains) all appear in the **agreement**
   list (§3c) with identical DER fingerprints. **No named real chain's
   anchor would be removed by adopting the Mozilla-derived set** -- but this
   is inference from anchor identity, not a re-run handshake, and is reported
   with that distinction intact rather than as an equivalent claim.

**Given (3), swapping the shipped bundle was not done.** The candidate
regenerates cleanly and passes every automated gate that touches it, but
"every automated gate that touches it" turned out, on inspection, not to
include the one this task cared most about -- which is itself the finding
this section exists to report, not something to quietly work around by
calling a different, easier gate sufficient.

## 5. The provenance gate

`check_provenance.py` (this directory, not wired into the Makefile -- the
Makefile is contended outside two specific lines this audit does not touch,
per the instructions this audit ran under) checks that every `*.pem` in
`tools/roots/` has its DER-SHA-256 fingerprint recorded in one of two
ledgers, both committed here:

- `certdata-fingerprints.txt` -- all 172 certificates in the fetched
  `certdata.txt` snapshot (§2), regardless of trust bit. Covers 88 of the
  130 (70 agreement + 18 present-but-not-server-trusted).
- `legacy-exceptions.txt` -- the 42 roots from §3a/A2, explicit and closed,
  each with its own recorded fingerprint. Adding to this file is meant to be
  a deliberate act (verify against a dated snapshot, write it down), not a
  place to drop an unexplained PEM.

```
$ python3 tools/roots/check_provenance.py
check_provenance: 130 PEM(s) all accounted for (88 in certdata-fingerprints.txt, 42 in legacy-exceptions.txt)
$ echo $?
0
```

### Negative control -- watched failing

A self-signed test certificate, sharing no fingerprint with either ledger,
was generated and dropped into `tools/roots/`:

```
$ openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout negctl.key -out tools/roots/zz_negctl_test_intruder.pem \
    -days 3 -nodes -subj "/CN=NEGCTL Intruder Test Root/O=Not A Real CA"
$ python3 tools/roots/check_provenance.py
check_provenance: 1 PEM(s) in tools/roots/ are UNACCOUNTED FOR (fingerprint not in certdata-fingerprints.txt or legacy-exceptions.txt):
  UNACCOUNTED  zz_negctl_test_intruder.pem: sha256=886cc8bfd18c44016fe56728d93fe805a8f3f7a366a5c9605d21bf46cf69dd65 matches neither ledger
Add it to the record deliberately (README.md explains how) before it ships, or remove it.
$ echo $?
1
```

The check named the exact file and exited non-zero. The intruder certificate
was then deleted (`git status` confirms `tools/roots/` holds only the
original 130 `*.pem`, this README, and the two ledgers) and the gate re-run
clean (130/130, exit 0) to confirm removal restores the pass.

## What this audit could not determine

- **Why** any specific root's trust was withdrawn, beyond the pattern visible
  in `CKA_TRUST_SERVER_AUTH` and `CKA_NSS_SERVER_DISTRUST_AFTER` -- Mozilla
  bug numbers or removal notices are not present in `certdata.txt` (checked,
  §2) and were not looked up on a second site, since only one network fetch
  was authorized for this audit.
- **Whether a real, currently-live chain for any of the 5-8 sites CLAUDE.md
  names** would still verify against the Mozilla-derived candidate bundle --
  no automated, network-free gate in this tree exercises the real compiled
  bundle against a captured real-world chain (§4.3), and re-proving it live
  would have meant HTTPS connections beyond the one fetch this task
  authorized. §4.4's anchor-identity argument is the closest available
  substitute, explicitly flagged as such.
- **The original 2026-06-08 import's exact source snapshot** -- which
  revision of macOS, or which exact `/etc/ssl/cert.pem`, produced the
  original 130. `THIRD_PARTY.md:159-161` already recorded this gap; nothing
  in this audit closed it, because the only new evidence gathered is what
  Mozilla ships TODAY, which is a different (and moving) target from
  whatever Apple shipped on 2026-06-08.
