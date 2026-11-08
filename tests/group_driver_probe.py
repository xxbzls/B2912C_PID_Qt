"""Installed IVI driver regression against loopback SCPI only; no hardware."""
import ctypes as c
import os
import socket
import threading
import time

root = os.environ.get('IVI_ROOT', r'C:\Program Files\IVI Foundation\IVI')
dll_directory = os.add_dll_directory(os.path.join(root, 'Bin'))
dll = c.WinDLL(os.path.join(root, 'Bin', 'KtB2900_64.dll'))
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen()
trace = []
group_state = b'(@1,2)'
server_errors = []


def serve():
    global group_state
    try:
        conn, _ = server.accept()
        with conn, conn.makefile('rb') as stream:
            for line in stream:
                text = line.decode('ascii').strip()
                if not text:
                    continue
                trace.append(text)
                upper = text.upper()
                if upper.startswith(':SYSTEM:GROUP:DEFINE '):
                    group_state = text.split(' ', 1)[1].encode()
                if '?' not in text:
                    continue
                if '*IDN?' in upper:
                    response = b'Keysight Technologies,B2912C,LOOPBACKTEST,6.0.516.0'
                elif 'GRO' in upper:
                    response = group_state
                elif 'ERR' in upper:
                    response = b'+0,"No error"'
                elif '*OPC?' in upper:
                    response = b'1'
                else:
                    assert upper == '*ESR?', f'Unexpected query: {text}'
                    response = b'0'
                conn.sendall(response[:3])
                time.sleep(.005)
                conn.sendall(response[3:])
                time.sleep(.005)
                conn.sendall(b'\n')
    except ConnectionResetError:
        pass
    except Exception as exc:
        server_errors.append(exc)


thread = threading.Thread(target=serve, daemon=True)
thread.start()
session = c.c_uint32()


def checked(status):
    if status < 0:
        code = c.c_int32()
        message = c.create_string_buffer(2048)
        dll.KtB2900_GetError(session, c.byref(code), len(message), message)
        raise RuntimeError(f'IVI {status}: {message.value!r}')


def write(command):
    count = c.c_int64()
    checked(dll.KtB2900_viWrite(session, c.c_int64(len(command)), command, c.byref(count)))
    assert count.value == len(command), f'Write count {count.value} != {len(command)}'


def raw_groups():
    start = len(trace)
    write(b':SYSTem:GROup:DEFine?')
    count = c.c_int64()
    buffer = c.create_string_buffer(256)
    checked(dll.KtB2900_SystemReadBytes(session, c.c_int64(len(buffer)), buffer, c.byref(count)))
    assert trace[start:] == [':SYSTem:GROup:DEFine?'], trace[start:]
    return buffer.raw[:count.value]


address = f'TCPIP0::127.0.0.1::{server.getsockname()[1]}::SOCKET'.encode()
try:
    checked(dll.KtB2900_InitWithOptions(address, 1, 0,
        b'Cache=false, Simulate=false, QueryInstrStatus=true', c.byref(session)))
    groups = c.create_string_buffer(256)
    checked(dll.KtB2900_SystemGetGroupedChannels(session, len(groups), groups))
    assert groups.value == b'(@1', groups.value
    print('PASS old getter reproduces screenshot: (@1,2) -> (@1')
    assert raw_groups() == b'(@1,2)'
    print('PASS raw read preserves coupled group; can correctly refuse it')
    write(b':SYSTem:GROup:DEFine (@1),(@2)')
    checked(dll.KtB2900_SystemWaitForOperationComplete(session, 2000))
    assert raw_groups() == b'(@1),(@2)'
    checked(dll.KtB2900_SystemGetGroupedChannels(session, len(groups), groups))
    assert groups.value == b'(@1)', groups.value
    print('PASS old getter also truncates independent group: (@1),(@2) -> (@1)')
    for _ in range(3):
        assert raw_groups() == b'(@1),(@2)'
    checked(dll.KtB2900_SystemWaitForOperationComplete(session, 2000))
    print('PASS explicit independent configuration and repeated full reads using installed driver')
    print('PASS fragmented responses assembled; no status queries inserted during raw query/read')
    assert not any('OUTP' in command.upper() or 'SOUR' in command.upper() for command in trace)
    print('PASS no output enable or current command; only loopback endpoint used')
finally:
    if session.value:
        checked(dll.KtB2900_close(session))
    server.close()
    thread.join(timeout=2)
assert not server_errors, server_errors
print('LIMIT real B2912C firmware and physical outputs were not tested')
