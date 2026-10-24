# sync.ps1 —— 极速启工具集 · 一键双备份同步
# 作用：把本机 main 分支的提交依次推送到 Gitee 与 GitHub 两个私有仓库。
# 入口：双击「同步到云端.bat」（它会用 -ExecutionPolicy Bypass 调用本文件）。
#
# 安全约定：
#   - 全程不出现任何令牌明文；推送认证由 Windows 凭据管理器（credential.helper=manager）自动完成。
#   - 绝不强制推送（--force）。检测到未提交改动时直接停止并提示。
#   - Gitee 走直连（显式清空代理）；GitHub 走系统默认代理（国内偶尔 502，带重试）。

$ErrorActionPreference = 'Continue'

# 无论在哪里双击，都先切到脚本所在目录（也就是仓库根）
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repo

function Banner($title) {
    Write-Host ""
    Write-Host ("=" * 60)
    Write-Host $title
    Write-Host ("=" * 60)
}

Banner "极速启工具集 · 双备份同步"
Write-Host "仓库目录 : $repo"
$branch = (git rev-parse --abbrev-ref HEAD 2>$null)
Write-Host "当前分支 : $branch"
$head = (git rev-parse --short HEAD 2>$null)
Write-Host "当前提交 : $head"

# 0) 未提交改动检查：有改动就停止，绝不强制推送
$dirty = git status --porcelain
if ($dirty) {
    Write-Host ""
    Write-Host "⚠ 检测到未提交的改动，已停止推送（绝不强制推送）：" -ForegroundColor Yellow
    $dirty | ForEach-Object { Write-Host "   $_" }
    Write-Host "请先在本机提交（或 git stash）后再运行本脚本。" -ForegroundColor Yellow
    Read-Host "按回车退出"
    exit 1
}

# 1) 推送单个远端（带重试 + 错误分类）
function Push-Remote($remote, $branch, $maxTry, $gitArgs) {
    for ($i = 1; $i -le $maxTry; $i++) {
        Write-Host ""
        Write-Host "→ 推送到 $remote ($branch)  [尝试 $i/$maxTry]"
        $out = git @gitArgs push $remote $branch 2>&1
        $rc  = $LASTEXITCODE
        if ($rc -eq 0) {
            Write-Host "✓ $remote 推送成功" -ForegroundColor Green
            return $true
        }
        $msg = ($out -join "`n")
        if ($msg -match "Permission denied|401|403|could not read Username") {
            Write-Host "✗ $remote 认证失败：令牌/凭据无效或已过期。" -ForegroundColor Red
            Write-Host "  处理：Windows 凭据管理器 → Windows 凭据，删除 gitee.com / github.com 相关项，" -ForegroundColor Red
            Write-Host "        重新生成私有令牌后用 'git credential approve' 存回，再双击本脚本。" -ForegroundColor Red
            return $false
        }
        elseif ($msg -match "failed to push some refs|non-fast-forward|rejected") {
            Write-Host "✗ $remote 被拒绝：远端有本地没有的提交（历史分叉）。" -ForegroundColor Red
            Write-Host "  处理：先 'git pull $remote $branch' 合并/变基后再同步；切勿强推。" -ForegroundColor Red
            return $false
        }
        elseif ($msg -match "502|504|timed out|Could not resolve|unable to access|Connection|SSL/TLS") {
            Write-Host "✗ $remote 网络异常（超时 / 502 / 握手失败）：第 $i 次失败。" -ForegroundColor Red
            if ($i -lt $maxTry) { Start-Sleep -Seconds 5; continue }
            Write-Host "  处理：检查网络/代理后重试；或稍后单独执行 'git push $remote $branch'。" -ForegroundColor Red
            return $false
        }
        else {
            Write-Host "✗ $remote 推送失败（未知原因），原始输出如下：" -ForegroundColor Red
            Write-Host $msg
            return $false
        }
    }
    return $false
}

# Gitee：直连稳定，重试 2 次；GitHub：国内网络偶尔 502，重试 4 次
# Gitee 显式清空 http(s).proxy 强制直连；GitHub 沿用系统代理（如有）。
$giteeArgs = @('-c', 'http.proxy=', '-c', 'https.proxy=', '-c', 'http.sslBackend=openssl')
$giteeOk = Push-Remote "gitee"  "main" 2 $giteeArgs
$ghOk    = Push-Remote "origin" "main" 4 @()

Banner "同步结果"
if ($giteeOk) { Write-Host "  Gitee  : ✓ 成功   https://gitee.com/xue-chong-sudo/jisuqi-tools"      -ForegroundColor Green } `
            else { Write-Host "  Gitee  : ✗ 失败" -ForegroundColor Red }
if ($ghOk)    { Write-Host "  GitHub : ✓ 成功   https://github.com/xue-chong-sudo/jisuqi-tools"     -ForegroundColor Green } `
            else { Write-Host "  GitHub : ✗ 失败" -ForegroundColor Red }

Write-Host ""
if ($giteeOk -and -not $ghOk) {
    Write-Host "提示：GitHub 暂时连不上，但 Gitee 已成功，代码已安全。" -ForegroundColor Cyan
    Write-Host "      等网络恢复后，直接双击本脚本即可把缺失的提交补推到 GitHub。" -ForegroundColor Cyan
}
elseif (-not $giteeOk -and $ghOk) {
    Write-Host "提示：Gitee 推送失败，但 GitHub 已成功，代码已安全。" -ForegroundColor Cyan
}
elseif (-not $giteeOk -and -not $ghOk) {
    Write-Host "提示：两个远端都失败，请按上面的处理建议排查后重试。" -ForegroundColor Red
}
else {
    Write-Host "两个远端均已同步完成 ✓" -ForegroundColor Green
}

Read-Host "按回车键关闭窗口"
