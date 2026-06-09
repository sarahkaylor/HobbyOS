import pexpect
import sys

child = pexpect.spawn('ssh -t -t -o StrictHostKeyChecking=no -i /Users/sarah/.ssh/mac_to_r1 root@192.168.10.174 "qm terminal 225"')
child.logfile = sys.stdout.buffer

def wait_for_prompt():
    for i in range(15):
        child.sendline('\r')
        idx = child.expect(['testvm login:', 'root@testvm:~#', 'root@testvm:', pexpect.TIMEOUT], timeout=3)
        if idx == 0:
            child.sendline('root')
            child.expect('Password:', timeout=3)
            child.sendline('root')
            child.expect(['root@testvm:~#', 'root@testvm:'], timeout=5)
            return True
        elif idx == 1 or idx == 2:
            return True
        print("Still waiting for prompt (attempt %d/15)..." % (i+1))
    return False

if not wait_for_prompt():
    print("Could not get prompt")
    sys.exit(1)

print("\n=== RUNNING LSPCI ===")
child.sendline('lspci -nn')
child.expect(['root@testvm:~#', 'root@testvm:'], timeout=10)

print("\n=== RUNNING DMESG ===")
child.sendline('dmesg | grep -i nvrm || dmesg | tail -n 50')
child.expect(['root@testvm:~#', 'root@testvm:'], timeout=10)

print("\n=== RUNNING NVIDIA-SMI ===")
child.sendline('nvidia-smi')
child.expect(['root@testvm:~#', 'root@testvm:'], timeout=20)

child.sendline('\x0f') # Ctrl+O to exit qm terminal
child.expect(pexpect.EOF)
