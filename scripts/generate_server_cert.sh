#!/usr/bin/env bash
set -euo pipefail

# Generate a self-signed TLS certificate and key for SMTPS server.
# Default output:
#   config/crt/server.crt
#   config/crt/server.key

usage() {
  cat <<'EOF'
Generate server.crt and server.key into config/crt.

Usage:
  ./scripts/generate_server_cert.sh [options]

Options:
  -d, --domain DOMAIN      Primary DNS name for certificate CN/SAN (default: localhost)
  -o, --out-dir DIR        Output directory (default: ./config/crt)
  -n, --cert-name NAME     Certificate file name (default: server.crt)
  -k, --key-name NAME      Private key file name (default: server.key)
  -y, --days DAYS          Validity period in days (default: 825)
  -b, --bits BITS          RSA key bits (default: 2048)
  -f, --force              Overwrite existing files
  -h, --help               Show help

Examples:
  ./scripts/generate_server_cert.sh
  ./scripts/generate_server_cert.sh -d mail.hgmail.xin
  ./scripts/generate_server_cert.sh -d mail.hgmail.xin -y 365 -f
EOF
}

DOMAIN="localhost"
OUT_DIR="./config/crt"
CERT_NAME="server.crt"
KEY_NAME="server.key"
DAYS="825"
BITS="2048"
FORCE="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d|--domain)
      DOMAIN="${2:-}"
      shift 2
      ;;
    -o|--out-dir)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    -n|--cert-name)
      CERT_NAME="${2:-}"
      shift 2
      ;;
    -k|--key-name)
      KEY_NAME="${2:-}"
      shift 2
      ;;
    -y|--days)
      DAYS="${2:-}"
      shift 2
      ;;
    -b|--bits)
      BITS="${2:-}"
      shift 2
      ;;
    -f|--force)
      FORCE="1"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$DOMAIN" ]]; then
  echo "Error: domain cannot be empty." >&2
  exit 1
fi

if ! [[ "$DAYS" =~ ^[0-9]+$ ]] || [[ "$DAYS" -lt 1 ]]; then
  echo "Error: --days must be an integer >= 1." >&2
  exit 1
fi

if ! [[ "$BITS" =~ ^[0-9]+$ ]] || [[ "$BITS" -lt 1024 ]]; then
  echo "Error: --bits must be an integer >= 1024." >&2
  exit 1
fi

if ! command -v openssl >/dev/null 2>&1; then
  echo "Error: openssl not found. Please install OpenSSL." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
CERT_PATH="${OUT_DIR}/${CERT_NAME}"
KEY_PATH="${OUT_DIR}/${KEY_NAME}"

if [[ "$FORCE" != "1" ]]; then
  for f in "$CERT_PATH" "$KEY_PATH"; do
    if [[ -e "$f" ]]; then
      echo "Error: file exists: $f (use --force to overwrite)" >&2
      exit 1
    fi
  done
fi

TMP_CONF="$(mktemp)"
cleanup() {
  rm -f "$TMP_CONF"
}
trap cleanup EXIT

cat > "$TMP_CONF" <<EOF
[req]
default_bits = ${BITS}
prompt = no
default_md = sha256
req_extensions = req_ext
x509_extensions = v3_ext
distinguished_name = dn

[dn]
C = CN
ST = Shanghai
L = Shanghai
O = MailSystem
OU = Backend
CN = ${DOMAIN}

[req_ext]
subjectAltName = @alt_names

[v3_ext]
authorityKeyIdentifier = keyid,issuer
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = ${DOMAIN}
DNS.2 = localhost
IP.1 = 127.0.0.1
EOF

echo "[1/2] Generating key and self-signed certificate..."
openssl req -x509 -nodes -newkey "rsa:${BITS}" \
  -keyout "$KEY_PATH" \
  -out "$CERT_PATH" \
  -days "$DAYS" \
  -config "$TMP_CONF" >/dev/null 2>&1

chmod 600 "$KEY_PATH"
chmod 644 "$CERT_PATH"

echo "[2/2] Done"
echo "Certificate: $CERT_PATH"
echo "Private key: $KEY_PATH"
echo "CN/SAN domain: $DOMAIN"

echo
echo "Update config if needed:"
echo "  \"certFile\": \"crt/${CERT_NAME}\""
echo "  \"keyFile\": \"crt/${KEY_NAME}\""
