# Unpack the OUTPUT.BIN of a VTech V.Flash disc (tested on Bratz Fashion Pixiez:
# The Secret Necklace) and turn its music into WAV.
#
#   python tools/output_bin_extract.py OUTPUT.BIN out
#
# OUTPUT.BIN: a u32 table length, then one entry per file - u32 sector (2048
# bytes), u32 size, u32 name length, the name ("\A2MEDIA\FULLMAINTHEME.AIF;1",
# NUL padded) - and the files themselves.
# The .AIF music is AIFF-C with Apple's IMA4 ADPCM codec (that is why Audacity
# only gives noise when it reads it as raw PCM). With ffmpeg on the PATH, each
# .AIF is also written as a .wav beside it; without it, ffmpeg or VLC play the
# .AIF files as they are.
import os, shutil, struct, subprocess, sys

src, out = sys.argv[1], sys.argv[2]
ffmpeg = shutil.which("ffmpeg")
with open(src, "rb") as f:
    toc_len = struct.unpack("<I", f.read(4))[0]
    toc = f.read(toc_len - 4)
    p = count = 0
    while p + 12 <= len(toc):
        sector, size, name_len = struct.unpack_from("<III", toc, p)
        name = toc[p + 12:p + 12 + name_len].split(b"\0")[0].decode("latin1")
        p += 12 + name_len
        if not name:
            break
        path = os.path.join(out, *name.split(";")[0].strip("\\").split("\\"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        f.seek(sector * 2048)
        with open(path, "wb") as o:
            o.write(f.read(size))
        count += 1
        if ffmpeg and path.upper().endswith(".AIF"):
            subprocess.run([ffmpeg, "-v", "error", "-y", "-i", path, path[:-4] + ".wav"])
print(count, "files written to", out)
