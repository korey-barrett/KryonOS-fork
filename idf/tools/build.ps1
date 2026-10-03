# Build the IDF project for the Korvo-1.
#
# Exists as a file rather than a one-liner because the activation profile is noisy and the quoting
# needed to survive cmd -> powershell -> idf.py in one line is where the output keeps getting lost.
# The profile is dot-sourced for its environment exports only; its banner is not this script's news.

. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'

Set-Location 'D:\KryonOS-fork\idf'
idf.py build
exit $LASTEXITCODE
