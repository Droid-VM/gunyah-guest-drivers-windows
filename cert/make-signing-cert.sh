#!/usr/bin/env bash
# =====================================================================
# make-signing-cert.sh — 一次性產生「固定」的 DroidVM 驅動測試簽章憑證，
# 並設成 GitHub secret。之後每次 CI（build-arm64-drivers.yml）都用同一張
# 憑證簽所有驅動 -> 跨 run 一致（解決「同名不同金鑰」造成的安裝提示）。
#
# 需求：OpenSSL 3（建議 brew：/opt/homebrew/bin/openssl）+ gh CLI（已登入正確帳號）
# 用法：bash cert/make-signing-cert.sh           # 預設密碼 droidvm
#       PFX_PW='你的密碼' bash cert/make-signing-cert.sh
# =====================================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PW="${PFX_PW:-droidvm}"
OUT="${OUT:-$HERE/DroidVM_Test}"   # 預設輸出到本腳本所在的 cert/（已被 .gitignore 排除）
SUBJ="/CN=DroidVM Test"

command -v openssl >/dev/null || { echo "需要 openssl（brew install openssl）"; exit 1; }

# 1) 自簽 code-signing 憑證（RSA 2048、10 年、含 codeSigning EKU）
#    basicConstraints=CA:FALSE 很關鍵：必須是 end-entity 憑證，Windows 才會把它當成
#    合法的程式碼簽署「發行者」。CA:TRUE（openssl req -x509 的預設）即使匯入 TrustedPublisher
#    也不消「無法驗證發行者」的驅動安裝提示。
openssl req -x509 -newkey rsa:2048 -keyout "$OUT.key" -out "$OUT.cer.pem" -days 3650 -nodes \
  -subj "$SUBJ" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning"

# 2) 打包成 Windows（signtool / Import-PfxCertificate）可匯入的 pfx。
#    OpenSSL 3 預設用 AES-256，部分 Windows 匯入會有問題 -> 用 -legacy（SHA1/3DES）最相容。
#    LibreSSL（無 -legacy）本來就是舊演算法，退回不帶旗標即可。
openssl pkcs12 -export -legacy -out "$OUT.pfx" -inkey "$OUT.key" -in "$OUT.cer.pem" -passout "pass:$PW" 2>/dev/null \
  || openssl pkcs12 -export        -out "$OUT.pfx" -inkey "$OUT.key" -in "$OUT.cer.pem" -passout "pass:$PW"

# 3) base64（單行，無換行）-> 供 gh secret 使用
base64 -i "$OUT.pfx" | tr -d '\n' > "$OUT.pfx.b64"

echo "----------------------------------------------------------------"
echo "已產生：$OUT.pfx（密碼：$PW）、$OUT.cer.pem（公鑰）"
echo "指紋（SHA1）：$(openssl x509 -in "$OUT.cer.pem" -noout -fingerprint -sha1 | sed 's/.*=//')"
echo
echo "設定 GitHub secret（在本 repo 目錄執行）："
echo "  gh secret set DROIDVM_PFX_BASE64 < $OUT.pfx.b64"
[ "$PW" = "droidvm" ] || echo "  gh secret set DROIDVM_PFX_PW --body '$PW'   # 你改了密碼才需要"
echo
echo "⚠️  $OUT.key / $OUT.pfx / *.b64 是私鑰，請妥善保管，勿提交進 git（已被 .gitignore 排除建議）。"
echo "----------------------------------------------------------------"
