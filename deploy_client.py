import pexpect
import sys
import time

child = pexpect.spawn('ssh -t -t -o StrictHostKeyChecking=no -i /Users/sarah/.ssh/mac_to_r1 root@192.168.10.174 "qm terminal 225"')
child.logfile = sys.stdout.buffer

def wait_for_prompt():
    for _ in range(15):
        child.sendline('\n')
        idx = child.expect(['testvm login:', 'root@testvm:~#', pexpect.TIMEOUT], timeout=5)
        if idx == 0:
            child.sendline('root')
            child.expect('Password:', timeout=5)
            child.sendline('root')
            child.expect('root@testvm:~#', timeout=5)
            return True
        elif idx == 1:
            return True
        print("Still waiting for prompt...")
    return False

if not wait_for_prompt():
    print("Could not get prompt")
    sys.exit(1)

child.sendline('dmesg | grep NVRM')
child.expect('root@testvm:~#')
print("dmesg NVRM:", child.before.decode())

child.sendline('dmesg | tail -n 50')
child.expect('root@testvm:~#')
print("dmesg tail:", child.before.decode())

child.sendline('lspci -nn')
child.expect('root@testvm:~#')
print("lspci:", child.before.decode())

child.sendline('\x0f') # Ctrl+O to exit qm terminal
child.expect(pexpect.EOF)

