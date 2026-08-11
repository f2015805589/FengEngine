# FEngine 项目守卫脚本
# 用途：在 CI / 本地操作前检查当前分支是否为受保护分支，防止误提交到 main/master。
# 用法：powershell -ExecutionPolicy Bypass -File scripts/guard.ps1 [-ProtectedBranch <string>...] [-AllowDetached]

[CmdletBinding()]
param(
    [string[]]$ProtectedBranch = @('main', 'master'),
    [switch]$AllowDetached
)

$ErrorActionPreference = 'Stop'

# 中文输出前设置 UTF-8 编码，避免乱码
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
} catch {
    # 无控制台环境下忽略
}

# git 不存在时退出 127
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCommand) {
    Write-Host '[GUARD] 未找到 git 命令，无法执行守卫检查。' -ForegroundColor Red
    exit 127
}

# 参数校验：受保护分支列表不能为空（参数错误 -> exit 3）
if ($null -eq $ProtectedBranch -or $ProtectedBranch.Count -eq 0 -or ($ProtectedBranch -join '').Length -eq 0) {
    Write-Host '[GUARD] 参数错误：-ProtectedBranch 不能为空。' -ForegroundColor Red
    exit 3
}

# 定位仓库根目录
$topLevel = & $gitCommand 'rev-parse' '--show-toplevel'
if ($LASTEXITCODE -ne 0) {
    Write-Host '[GUARD] 当前目录不是 git 仓库，拒绝继续。' -ForegroundColor Red
    exit 2
}

# 获取当前分支（detached HEAD 时 git 输出为空，$branchOutput 为 $null）
$branchOutput = & $gitCommand 'branch' '--show-current'
if ($LASTEXITCODE -ne 0) {
    Write-Host '[GUARD] git branch --show-current 执行失败，拒绝继续。' -ForegroundColor Red
    exit 2
}
$currentBranch = ''
if ($null -ne $branchOutput) {
    $currentBranch = ([string]$branchOutput).Trim()
}

# detached HEAD 处理
if ([string]::IsNullOrEmpty($currentBranch)) {
    if ($AllowDetached) {
        Write-Host '[GUARD] 通过（detached HEAD，已由 -AllowDetached 放行）' -ForegroundColor Yellow
        exit 0
    }
    Write-Host '[GUARD] 当前处于 detached HEAD 状态，请切换到 feature 分支后再继续。' -ForegroundColor Red
    exit 2
}

# 受保护分支检查
if ($ProtectedBranch -contains $currentBranch) {
    $protectedList = $ProtectedBranch -join ', '
    Write-Host "[GUARD] 拒绝：当前分支 '$currentBranch' 是受保护分支（保护列表：$protectedList）。" -ForegroundColor Red
    Write-Host '[GUARD] 请切换到 feature 分支（如 git checkout -b feature/xxx）后再继续。' -ForegroundColor Red
    exit 1
}

Write-Host "[GUARD] 通过：当前分支 '$currentBranch'。" -ForegroundColor Green
exit 0
