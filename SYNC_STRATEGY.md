# 同步策略：hermes-agent (Bitbucket) → new_agent2 (Pi2 minimal)

## Pi2 專屬檔案（保留）
- `RASPBERRY_PI2_MANUAL.md` - Pi2 安裝手冊
- `README_PI2.md` - Pi2 摘要
- `setup-hermes.sh` - Pi2 專用安裝腳本（ARMv7 32-bit, 1GB RAM）
- `constraints-termux.txt` - Termux 平台限制

## 核心同步策略

### 1. 分層合併 (Layered Merge)

```bash
# 在 new_agent2 中
git fetch hermes-bitbucket main
git merge hermes-bitbucket/main --allow-unrelated-histories -m 'Merge hermes-agent main'
```

### 2. 合併後清理 (Post-Merge Cleanup)

#### 刪除不需要的桌面/Web 內容
```bash
# 刪除 apps/desktop/
rm -rf apps/desktop/

# 刪除 website/ (文件網站)
rm -rf website/

# 刪除 docs/ (除非需要)
# rm -rf docs/
```

#### 保留 Pi2 專用檔案
```bash
# 確認 Pi2 專用檔案存在
ls RASPBERRY_PI2_MANUAL.md README_PI2.md setup-hermes.sh constraints-termux.txt
```

#### 修改 `setup-hermes.sh`
```bash
# 移除 is_raspberry_pi2() 函數
# 移除 Pi2 專用的 minimal deps 安裝流程
# 使用標準 uv sync 流程
```

### 3. 更新 `pyproject.toml`
```toml
# 恢復完整版本
version = "0.17.0"  # 從 hermes-bitbucket

# 保留 Pi2 相關的 dependencies 修改
# 例如：numpy<2 for ARMv7 compatibility
```

## 同步檢查清單

### ✅ 必須同步
- [ ] `pyproject.toml` - dependency version update
- [ ] `setup.py` - build configuration
- [ ] `hermes_constants.py` - version/constants
- [ ] `agent/` - core agent loop
- [ ] `gateway/` - platform adapters
- [ ] `hermes_cli/` - CLI commands
- [ ] `tools/` - tool implementations
- [ ] `plugins/` - provider/platform plugins

### ⚠️ 選擇性同步
- [ ] `docs/` - 文檔 (可選)
- [ ] `scripts/` - 腳本 (可選)
- [ ] `.github/workflows/` - CI/CD (可選)

### ❌ 禁止同步
- [ ] `apps/desktop/` - 桌面應用
- [ ] `website/` - 文件網站
- [ ] `optional-skills/` - 大型 optional skills (除非 Pi2 需要)

## 修正合併衝突策略

### 1. README.md
- 保留 hermes-bitbucket 的完整說明
- 新增 Pi2 專用章節：`# Raspberry Pi 2 (ARMv7 32-bit)`

### 2. Dockerfile
- 保留 hermes-bitbucket 的 Dockerfile
- 新增 Pi2 專用 build stage

### 3. setup-hermes.sh
- 使用 hermes-bitbucket 的版本
- 新增 Pi2 detection + minimal deps 安裝

### 4. agent/agent_init.py
- hermes-bitbucket 的版本 (0.17.0)
- 確認 `is_raspberry_pi2()` 檢查

## 更新流程 (Script)

```bash
# 1. 在 hermes-agent (Bitbucket)
cd ~/work/Bitbucket/hermes-agent
git checkout main
git pull origin main

# 2. 在 new_agent2
cd ~/work/new_agent2
git fetch hermes-bitbucket main
git merge hermes-bitbucket/main --allow-unrelated-histories

# 3. 解決衝突後清理
git checkout --theirs hermes-bitbucket/main -- .
rm -rf apps/desktop/
rm -rf website/
# 調整 setup-hermes.sh, pyproject.toml

# 4. 提交並推送
git add .
git commit -m "Merge hermes-agent 0.17.0 + Pi2 minimal"
git push origin main
git tag v10.0-pi2
git push origin v10.0-pi2
```

## 版本對應

| new_agent2 Version | hermes-bitbucket Version | Notes |
|-------------------|--------------------------|-------|
| v0.9-pi2 | hermes-agent 0.16.0 | Current |
| v10.0-pi2 | hermes-agent 0.17.0 | Next sync target |

## 後續更新

每次 hermes-bitbucket 新版本釋出時：

1. `git fetch hermes-bitbucket main`
2. 檢查 `git log hermes-bitbucket/main --oneline -10`
3. 確認關鍵功能更新
4. 執行 merge + cleanup
5. 推送到 GitHub + 新增 tag
