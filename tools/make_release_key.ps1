<#
make_release_key.ps1 - creates VBoy Color's Android release signing key, once.

Android only installs an APK over an existing one when both are signed with
the same key. This makes that key and sets it up in both places that build
APKs:

  * your PC: the key goes to a folder OUTSIDE the repo (default
    %USERPROFILE%\VBoyColor-signing), as android.keystore + keystore.txt
    (the password), and android\local.properties gets keystore.dir pointing
    there - so your own release builds are signed with it;
  * GitHub: the repository secrets ANDROID_KEYSTORE_BASE64 and
    ANDROID_STORE_PASSWORD, so the CI's APKs (and the releases) are too -
    set for you with the GitHub CLI (gh) when it's installed and logged in,
    otherwise the script copies the values to the clipboard one at a time
    and opens the page to paste them into.

BACK UP THAT FOLDER (a USB stick, a password manager, ...). Lose it and no
future update can install over the copies people already have - they'd
have to uninstall first. Never put it in the repo (.gitignore keeps
*.keystore and keystore.txt out).

Run it from anywhere:
  powershell -ExecutionPolicy Bypass -File tools\make_release_key.ps1
Running it again keeps the existing key (it only redoes the setup steps).
#>
param(
    [string]$KeyDir = (Join-Path $env:USERPROFILE "VBoyColor-signing"),
    [string]$Repo = "yuk27/VBoyColor"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$keystore = Join-Path $KeyDir "android.keystore"
$passwordFile = Join-Path $KeyDir "keystore.txt"
$utf8 = New-Object System.Text.UTF8Encoding $false   # no BOM (Gradle reads these)

function Read-Secret([string]$prompt) {
    $secure = Read-Host $prompt -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# 1. The key (only if there isn't one yet - never replace a key in use).
if (Test-Path $keystore) {
    Write-Host "Keeping the existing key: $keystore"
    if (-not (Test-Path $passwordFile)) { throw "$passwordFile is missing - put the key's password on its first line." }
    $password = ([IO.File]::ReadAllLines($passwordFile) | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
} else {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += (Join-Path $env:JAVA_HOME "bin\keytool.exe") }
    $candidates += "$env:ProgramFiles\Android\Android Studio\jbr\bin\keytool.exe"
    $candidates += "$env:ProgramFiles\Android\Android Studio\jre\bin\keytool.exe"
    $candidates += "$env:LOCALAPPDATA\Programs\Android Studio\jbr\bin\keytool.exe"
    $onPath = Get-Command keytool -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $keytool = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $keytool) { throw "keytool not found - it comes with Android Studio (or set JAVA_HOME to a JDK)." }

    Write-Host "Creating VBoy Color's release key in $KeyDir"
    Write-Host "Pick a password (6+ characters) and keep it with the backup of that folder."
    $password = Read-Secret "Password"
    if ($password.Length -lt 6) { throw "The password needs at least 6 characters." }
    if ($password -ne (Read-Secret "Same password again")) { throw "The two passwords don't match." }

    New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null
    $env:VBC_NEW_KEY_PASSWORD = $password   # (via the environment, not the command line)
    try {
        & $keytool -genkeypair -keystore $keystore -storetype PKCS12 -alias key0 `
            -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=VBoy Color, OU=yuk27" `
            -storepass:env VBC_NEW_KEY_PASSWORD -keypass:env VBC_NEW_KEY_PASSWORD
        if ($LASTEXITCODE -ne 0) { throw "keytool failed (exit code $LASTEXITCODE)." }
    } finally {
        Remove-Item Env:\VBC_NEW_KEY_PASSWORD -ErrorAction SilentlyContinue
    }
    [IO.File]::WriteAllLines($passwordFile, [string[]]@($password), $utf8)
    Write-Host "Key created."
}

# 2. Your own builds: keystore.dir in android\local.properties.
$localProps = Join-Path $repoRoot "android\local.properties"
$comment = "# Release signing key (tools/make_release_key.ps1)"
$lines = @()
if (Test-Path $localProps) {
    $lines = @([IO.File]::ReadAllLines($localProps) | Where-Object { $_ -notmatch '^\s*keystore\.dir\s*=' -and $_ -ne $comment })
}
$lines += $comment
$lines += "keystore.dir=" + ($KeyDir -replace '\\', '/')
[IO.File]::WriteAllLines($localProps, [string[]]$lines, $utf8)
Write-Host "android\local.properties: keystore.dir set - your release builds now use this key."

# 3. GitHub: the repository secrets the CI signs with.
$keyBase64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($keystore))
$gh = Get-Command gh -ErrorAction SilentlyContinue
$ghReady = $false
if ($gh) { & gh auth status *> $null; $ghReady = ($LASTEXITCODE -eq 0) }
if ($ghReady) {
    & gh secret set ANDROID_KEYSTORE_BASE64 --repo $Repo --body $keyBase64
    if ($LASTEXITCODE -ne 0) { throw "gh couldn't set ANDROID_KEYSTORE_BASE64." }
    & gh secret set ANDROID_STORE_PASSWORD --repo $Repo --body $password
    if ($LASTEXITCODE -ne 0) { throw "gh couldn't set ANDROID_STORE_PASSWORD." }
    Write-Host "GitHub secrets set on $Repo."
} else {
    $page = "https://github.com/$Repo/settings/secrets/actions/new"
    Write-Host ""
    Write-Host "Now the two GitHub secrets ($page):"
    Set-Clipboard -Value $keyBase64
    Start-Process $page
    Write-Host "  1. Name: ANDROID_KEYSTORE_BASE64   Secret: paste (it's on the clipboard), then 'Add secret'."
    Read-Host "     Press Enter once it's added" | Out-Null
    Set-Clipboard -Value $password
    Start-Process $page
    Write-Host "  2. Name: ANDROID_STORE_PASSWORD    Secret: paste (the password is on the clipboard now), then 'Add secret'."
    Read-Host "     Press Enter once it's added" | Out-Null
    Set-Clipboard -Value " "
}

Write-Host ""
Write-Host "Done. Every APK from now on - yours and the CI's - installs over the previous one."
Write-Host "Back up $KeyDir somewhere safe: without it, updates can't install over existing copies."
