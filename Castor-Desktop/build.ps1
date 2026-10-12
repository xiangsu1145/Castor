$ErrorActionPreference = "Stop"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Castor Desktop - CMake + Clang 构建系统" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 1. 查找并加载 VS 环境变量
$vcvars = "G:\vis1\VC\Auxiliary\Build\vcvars64.bat"
if (Test-Path $vcvars) {
    Write-Host "[1/3] 加载 Windows SDK & MSVC 环境..." -ForegroundColor Yellow
    cmd /c "call `"$vcvars`" && set" | ForEach-Object {
        if ($_ -match "^(.*?)=(.*)$") {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

# 2. 补齐 CMake / Clang / Ninja 路径
$extraPaths = @(
    "G:\cmake\cmake-3.29.2-windows-x86_64\bin",
    "D:\llvm20\bin",
    "G:\python3.11.9\Scripts"
)
foreach ($p in $extraPaths) {
    if (Test-Path $p) {
        $env:PATH = "$p;$env:PATH"
    }
}

# 自动关闭正在运行的实例以避免 lld-link 写锁
Stop-Process -Name CastorDesktop -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200

$buildDir = "D:\Castor\Castor-Desktop\build"
if (!(Test-Path $buildDir)) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
}

Set-Location $buildDir

Write-Host "[2/3] 正在通过 CMake + Clang 配置工程..." -ForegroundColor Yellow
& cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="D:/llvm20/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="D:/llvm20/bin/clang-cl.exe" ..

if ($LASTEXITCODE -ne 0) {
    Write-Host "[重试] 尝试系统默认编译器..." -ForegroundColor Yellow
    & cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake 配置失败"
        exit 1
    }
}

Write-Host "[3/3] 正在通过 Ninja 编译..." -ForegroundColor Yellow
& ninja

if ($LASTEXITCODE -eq 0) {
    Write-Host "======================================================" -ForegroundColor Green
    Write-Host "[成功] 编译完成: $buildDir\CastorDesktop.exe" -ForegroundColor Green
    Write-Host "======================================================" -ForegroundColor Green
} else {
    Write-Error "编译失败"
    exit 1
}
