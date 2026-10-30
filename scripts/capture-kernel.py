#!/usr/bin/env python3
"""Capture new /dev/kmsg records; reject sequence gaps and buffer overruns."""
import os,select,sys
from pathlib import Path
log,ready,end=sys.argv[1:]
fd=os.open('/dev/kmsg',os.O_RDONLY|os.O_NONBLOCK)
try:
 os.lseek(fd,0,os.SEEK_END)
 previous=None
 with open(log,'w') as out:
  Path(ready).touch()
  while True:
   if not select.select([fd],[],[],1)[0]: continue
   try: record=os.read(fd,1048576)
   except BlockingIOError: continue
   header,body=record.decode(errors='replace').split(';',1)
   fields=header.split(','); seq=int(fields[1]); timestamp=int(fields[2])/1000000
   if previous is not None and seq!=previous+1:
    raise RuntimeError(f'Kernel log sequence gap: {previous} -> {seq}')
   previous=seq
   out.write(f'[{timestamp:.6f}] {body}')
   if not body.endswith('\n'): out.write('\n')
   out.flush()
   if body.splitlines()[0]==end: break
finally:
 os.close(fd)
