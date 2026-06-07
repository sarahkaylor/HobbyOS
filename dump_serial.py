import pexpect
import sys

child = pexpect.spawn('ssh -t -t -o StrictHostKeyChecking=no -i /Users/sarah/.ssh/mac_to_r1 root@192.168.10.174 "qm terminal 225"')
f = open('serial_dump.txt', 'wb')
child.logfile = f

child.sendline('\n')
try:
    child.expect('SOMETHING_IMPOSSIBLE', timeout=10)
except pexpect.TIMEOUT:
    pass

child.sendline('\n')
try:
    child.expect('SOMETHING_IMPOSSIBLE', timeout=10)
except pexpect.TIMEOUT:
    pass

child.sendline('\x0f')
try:
    child.expect(pexpect.EOF, timeout=2)
except:
    pass
f.close()
