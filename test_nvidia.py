import pexpect
import sys

child = pexpect.spawn('ssh -t -t -o StrictHostKeyChecking=no -i /Users/sarah/.ssh/mac_to_r1 root@192.168.10.174 "qm terminal 225"')
child.logfile = sys.stdout.buffer

try:
    child.expect('testvm login:', timeout=10)
    child.sendline('root')
    child.expect('Password:', timeout=10)
    child.sendline('root')
except pexpect.TIMEOUT:
    child.sendline('\n')
    try:
        child.expect('root@testvm:', timeout=5)
    except pexpect.TIMEOUT:
        print("Could not find prompt")

child.sendline('nvidia-smi')
child.expect('root@testvm:', timeout=20)
child.sendline('\x0f') # Ctrl+O to exit qm terminal
child.expect(pexpect.EOF)
