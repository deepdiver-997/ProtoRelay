#!/usr/bin/env bash
set -euo pipefail

# Generate DKIM private/public keys and a DNS TXT record snippet.
# Usage:
#   ./scripts/generate_dkim_keys.sh -d example.com -s default [-o ./config/dkim] [-b 2048]

usage() {
  cat <<'EOF'
Generate DKIM keys and DNS TXT content.

Required:
  -d, --domain DOMAIN        DKIM domain, e.g. example.com
  -s, --selector SELECTOR    DKIM selector, e.g. default

Optional:
  -o, --out-dir DIR          Output directory (default: ./config/dkim)
  -b, --bits N               RSA key bits (default: 2048)
  -f, --force                Overwrite existing files
  -h, --help                 Show help

Outputs:
  <out-dir>/<selector>.<domain>.private.pem
  <out-dir>/<selector>.<domain>.public.pem
  <out-dir>/<selector>.<domain>.dns.txt

DNS record host:
  <selector>._domainkey.<domain>
EOF
}

DOMAIN=""
SELECTOR=""
OUT_DIR="./config/dkim"
BITS="2048"
FORCE="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d|--domain)
      DOMAIN="${2:-}"
      shift 2
      ;;
    -s|--selector)
      SELECTOR="${2:-}"
      shift 2
      ;;
    -o|--out-dir)
      OUT_DIR="${2:-}"
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

if [[ -z "$DOMAIN" || -z "$SELECTOR" ]]; then
  echo "Error: --domain and --selector are required." >&2
  usage
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
BASENAME="${SELECTOR}.${DOMAIN}"
PRIVATE_KEY_PATH="${OUT_DIR}/${BASENAME}.private.pem"
PUBLIC_KEY_PATH="${OUT_DIR}/${BASENAME}.public.pem"
DNS_TXT_PATH="${OUT_DIR}/${BASENAME}.dns.txt"

if [[ "$FORCE" != "1" ]]; then
  for f in "$PRIVATE_KEY_PATH" "$PUBLIC_KEY_PATH" "$DNS_TXT_PATH"; do
    if [[ -e "$f" ]]; then
      echo "Error: file exists: $f (use --force to overwrite)" >&2
      exit 1
    fi
  done
fi

echo "[1/4] Generating private key: $PRIVATE_KEY_PATH"
openssl genrsa -out "$PRIVATE_KEY_PATH" "$BITS" >/dev/null 2>&1
chmod 600 "$PRIVATE_KEY_PATH"

echo "[2/4] Generating public key: $PUBLIC_KEY_PATH"
openssl rsa -in "$PRIVATE_KEY_PATH" -pubout -out "$PUBLIC_KEY_PATH" >/dev/null 2>&1

# Extract base64 payload from PEM public key.
PUB_B64="$(awk 'BEGIN{p=0} /BEGIN PUBLIC KEY/{p=1;next} /END PUBLIC KEY/{p=0} p{printf "%s",$0}' "$PUBLIC_KEY_PATH")"
if [[ -z "$PUB_B64" ]]; then
  echo "Error: failed to extract public key payload." >&2
  exit 1
fi

RECORD_HOST="${SELECTOR}._domainkey.${DOMAIN}"
RECORD_VALUE="v=DKIM1; k=rsa; p=${PUB_B64}"

echo "[3/4] Writing DNS TXT helper file: $DNS_TXT_PATH"
{
  echo "# Add this TXT record to your DNS provider"
  echo "host: ${RECORD_HOST}"
  echo "type: TXT"
  echo "value: ${RECORD_VALUE}"
  echo
  echo "# Some providers require only host label (without root domain)."
  echo "# If so, use host: ${SELECTOR}._domainkey"
} > "$DNS_TXT_PATH"

echo "[4/4] Done"
echo
echo "Private key: $PRIVATE_KEY_PATH"
echo "Public key : $PUBLIC_KEY_PATH"
echo "DNS helper : $DNS_TXT_PATH"
echo
echo "Fill your server config with:"
echo "  outbound_dkim_enabled: true"
echo "  outbound_dkim_selector: ${SELECTOR}"
echo "  outbound_dkim_domain: ${DOMAIN}"
echo "  outbound_dkim_private_key_file: ${PRIVATE_KEY_PATH}"
