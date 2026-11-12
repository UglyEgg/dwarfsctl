#!/usr/bin/env python3
"""Exercise actual offscreen font/image rendering, not merely compilation."""
import os, struct, subprocess, sys, tempfile
from pathlib import Path
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp); image=root/'sample.dwarfs'; image.touch(); output=root/'thumb.png'
    env=dict(os.environ, HOME=tmp, XDG_RUNTIME_DIR=tmp, DBUS_SESSION_BUS_ADDRESS='unix:path=/no/bus')
    env.pop('QT_QPA_PLATFORM',None)
    result=subprocess.run([sys.argv[1],str(image),str(output),'128'],env=env,capture_output=True,timeout=10)
    assert result.returncode==0,(result.returncode,result.stderr)
    data=output.read_bytes()
    assert data[:8]==b'\x89PNG\r\n\x1a\n' and struct.unpack('>II',data[16:24])==(128,128)
    for size in ['0','-1','1000000','no']:
        result=subprocess.run([sys.argv[1],str(image),str(output),size],env=env,capture_output=True,timeout=10)
        assert result.returncode==2,(size,result.returncode,result.stderr)
print('Thumbnail runtime contracts passed')
