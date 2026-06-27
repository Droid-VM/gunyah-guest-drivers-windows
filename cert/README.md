# cert/ — 驅動簽章測試憑證

CI（`.github/workflows/build-arm64-drivers.yml`）用一張**固定的自簽 code-signing 憑證**
（`CN=DroidVM Test`）簽署所有 ARM64 驅動。把這張憑證的 `.pfx` 放進 GitHub secret，
**每次 build 都用同一張憑證簽** → 跨 run 一致，避免「同名不同金鑰」造成 guest 安裝時跳
「無法驗證發行者」。

> 這是**測試簽章**（self-signed），只在 guest 開了 `testsigning` 並把憑證匯入
> `Root + TrustedPublisher` 後才受信任。不是正式 WHQL 簽章。

## 這個資料夾追蹤什麼

| 檔案 | git | 說明 |
|---|---|---|
| `README.md`、`make-signing-cert.sh` | ✅ 追蹤 | 文件與產生腳本 |
| `DroidVM_Test.pfx` / `.key` / `.cer.pem` / `.pfx.b64` | 🚫 ignore | 私鑰材料，**勿提交**（見根 `.gitignore`） |

## 1) 產生憑證

```bash
bash cert/make-signing-cert.sh            # 預設密碼 droidvm
# 或自訂密碼：
PFX_PW='你的密碼' bash cert/make-signing-cert.sh
```

產物會落在本資料夾（`cert/`）：

- `DroidVM_Test.pfx` — 含私鑰，**簽章用**（密碼預設 `droidvm`）
- `DroidVM_Test.cer.pem` — 公鑰
- `DroidVM_Test.pfx.b64` — pfx 的 base64（給 GitHub secret 用）
- `DroidVM_Test.key` — 私鑰

腳本結尾會印出該憑證的 **SHA1 指紋**與要執行的 `gh secret set` 指令。

需求：`openssl`（建議 OpenSSL 3，brew）+ `gh` CLI（已登入有 repo 權限的帳號）。

## 2) 設定到 GitHub Actions

```bash
# 必填：pfx 的 base64
gh secret set DROIDVM_PFX_BASE64 < cert/DroidVM_Test.pfx.b64

# 選填：只有當你把密碼從預設 droidvm 改掉時才需要
gh secret set DROIDVM_PFX_PW --body '你的密碼'
```

workflow 的行為：

- `build-arm64-drivers.yml` 的 **Sign + package** step：有 `DROIDVM_PFX_BASE64` 就還原成
  pfx 簽署；**沒有則退回每次新生的自簽憑證**（會印 warning，跨 run 憑證不同）。
  密碼優先用 secret `DROIDVM_PFX_PW`，否則用預設 `droidvm`。
- `dev-release.yml` 透過 `uses:` 呼叫上面那個 reusable workflow，並加了 **`secrets: inherit`**
  把 secret 傳進去（reusable workflow 不會自動繼承呼叫端的 secret，漏了就會退回自簽）。

## 3) 驗證 CI 真的用了這張憑證

下載任一次 build 的產物，檢查 `.cat` 的簽章者指紋是否 == 你的憑證：

```bash
gh run download <run-id> -n droidvm-arm64-drivers -D /tmp/dl
openssl pkcs7 -inform DER -in /tmp/dl/drivers/vioinput/vioinput.cat -print_certs \
  | openssl x509 -noout -fingerprint -sha1
# 應等於 make-signing-cert.sh 印出的 SHA1 指紋
```

## 注意

- **備份 `DroidVM_Test.pfx`**：要重設 secret 或本機手動簽署時需要它。弄丟就重跑腳本
  產一張新的並重設 secret（憑證會換、需重簽）。
- 固定憑證的代價：私鑰一旦外洩可被冒簽。**勿把 pfx/key 推上公開 repo**（已 gitignore）。
