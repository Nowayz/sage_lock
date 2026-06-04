# Generates sage_lock.ico by extracting the Windows Segoe UI Emoji COLR/CPAL lock glyph.
# The Python script does the actual color-layer/gradient rendering.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
python (Join-Path $scriptDir 'GenerateSageLockIcon.py')
