"""Real installed driver + local HiSLIP wire inspection. No external hardware."""
import ctypes as c
import os
from loopback_hislip import HiSlipServer
handle = os.add_dll_directory(r'C:\Program Files\IVI Foundation\IVI\Bin')
dll = c.WinDLL(r'C:\Program Files\IVI Foundation\IVI\Bin\KtB2900_64.dll')
commands = []
group_state = b'(@1,2)'
allow_duplicate = False

def respond(command):
    global group_state
    commands.append(command)
    if not allow_duplicate:
        assert command.endswith(b'\n') and command.count(b'\n') == 1, repr(command)
    upper = command.decode().upper().strip()
    if upper == ':TEST:TIMEOUT?': return None  # Deliberately leave the query unanswered.
    if '*IDN?' in upper: return b'Keysight Technologies,B2912C,LOOPBACKTEST,6.0.516.0\n'
    if upper.startswith(':SYSTEM:GROUP:DEFINE '): group_state = command.strip().split(b' ', 1)[1]
    if 'GRO' in upper and '?' in upper: return group_state+b'\n'
    if 'ERR?' in upper: return b'+0,"No error"\n'
    if '*OPC?' in upper: return b'1\n'
    if '?' in upper: return b'0\n'
    return None

server = HiSlipServer(respond)
session=c.c_uint32()
address=f'TCPIP0::127.0.0.1::hislip0,{server.port}::INSTR'.encode()

def checked(status):
    if status < 0:
        code=c.c_int32(); msg=c.create_string_buffer(2048)
        dll.KtB2900_GetError(session,c.byref(code),len(msg),msg)
        raise RuntimeError(f'{status}: {msg.value!r}')

def write(command):
    written=c.c_int64()
    checked(dll.KtB2900_viWrite(session,c.c_int64(len(command)),command,c.byref(written)))
    assert written.value == len(command)

def query(command):
    write(command)
    received=c.c_int64(); buffer=c.create_string_buffer(256)
    checked(dll.KtB2900_SystemReadBytes(session,c.c_int64(len(buffer)),buffer,c.byref(received)))
    assert 0 < received.value < len(buffer)
    return buffer.raw[:received.value]

try:
    checked(dll.KtB2900_InitWithOptions(address,1,0,b'Cache=false, Simulate=false, QueryInstrStatus=true',c.byref(session)))
    # Reproduce the old wire defect without touching a real device.
    allow_duplicate = True
    assert query(b':SYSTem:GROup:DEFine?\n') == b'(@1,2)'
    assert commands[-1] == b':SYSTem:GROup:DEFine?\n\n'
    print('PASS previous call sends TWO LF bytes over HiSLIP')
    allow_duplicate = False
    first = len(commands)
    write(b':SYSTem:GROup:DEFine (@1),(@2)')
    checked(dll.KtB2900_SystemWaitForOperationComplete(session,2000))
    for _ in range(4):
        index = len(commands)
        assert query(b':SYSTem:GROup:DEFine?') == b'(@1),(@2)'
        assert commands[index:] == [b':SYSTem:GROup:DEFine?\n']
    checked(dll.KtB2900_SystemWaitForOperationComplete(session,2000))
    assert all(command.count(b'\n') == 1 for command in commands[first:])
    print('PASS corrected configuration/query each has ONE LF; repeated readback completes over HiSLIP')
    # KtB2900.h: IVI_SPECIFIC_ATTR_BASE + 5, milliseconds.
    checked(dll.KtB2900_SetAttributeViInt32(session,b'',1_150_005,200))
    write(b':TEST:TIMEOUT?')
    count=c.c_int64(); buffer=c.create_string_buffer(256)
    status=dll.KtB2900_SystemReadBytes(session,c.c_int64(len(buffer)),buffer,c.byref(count))
    assert status < 0, 'Missing response must produce an I/O failure'
    code=c.c_int32(); message=c.create_string_buffer(2048)
    dll.KtB2900_GetError(session,c.byref(code),len(message),message)
    print('PASS injected missing response produces read timeout')
    checked(dll.KtB2900_SystemClearIO(session))
    assert any(packet[0] == 19 for packet in server.trace)
    assert any(packet[0] == 8 for packet in server.trace)
    assert query(b':SYSTem:GROup:DEFine?') == b'(@1),(@2)'
    print('PASS after timeout Device Clear restores query/response synchronization and preserves grouping')
    assert not any(b'OUTP' in cmd.upper() or b'SOUR' in cmd.upper() for cmd in commands)
    assert not server.errors, server.errors
    print('PASS tests send no output enable/current commands; endpoint is loopback-only')
finally:
    if session.value: dll.KtB2900_close(session)
    server.close()
print('LIMIT: mock HiSLIP peer; B2912C firmware and physical output behavior need bench verification')
