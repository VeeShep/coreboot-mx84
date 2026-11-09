#!/usr/bin/env python3
"""Live bridge acceptance: connect only one router-facing data cable; retain Management.
Captures only counts for the test MAC, not packet contents. Not physical-jack isolation proof.
"""
import socket,select,subprocess,time,signal,os,sys,struct,json,runpy
from pathlib import Path
allowed={'WAN1','WAN2',*(f'LAN{i}' for i in range(1,9))}
if len(sys.argv)!=2 or sys.argv[1] not in allowed:
    raise SystemExit('usage: test-copper-isolation.py WAN1|WAN2|LAN1..LAN8')
if os.geteuid()!=0:
    raise SystemExit('run as root on Fry')
label=sys.argv[1]
target='br-'+label.lower()
backend=runpy.run_path('/usr/lib/mx84-platform/mx84-switch-backend')
physical=backend['load_config']('/etc/mx84/ports.conf')[label]
def counters():
    payload,_=json.JSONDecoder().raw_decode(backend['ctl']('stats',physical).lstrip())
    values=payload[f'port{physical}']
    required=('bytes_rx','bytes_tx','packets_rx','packets_tx','errs_rx','errs_tx','drop_rx','drop_tx')
    assert all(type(values[key]) is int and values[key]>=0 for key in required)
    return {key:values[key] for key in required}
before=counters()
labels=['wan1','wan2']+[f'lan{i}' for i in range(1,9)]+['sfp1','sfp2','pfctl']
source=bytes.fromhex('52540084fe01')
sockets={}
counts={f'br-{label}':0 for label in labels}
process=None
try:
    for bridge in counts:
        channel=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3))
        channel.bind((bridge,0))
        channel.setblocking(False)
        sockets[channel]=bridge
    process=subprocess.Popen(['sh',str(Path(__file__).with_name('test-copper-path.sh')),label],start_new_session=True)
    deadline=time.monotonic()+90
    finished=None
    while True:
        if time.monotonic()>deadline: raise TimeoutError('port test exceeded deadline')
        ready,_,_=select.select(list(sockets),[],[],0.1)
        for channel in ready:
            frame=channel.recv(65535)
            if len(frame)>=14 and frame[6:12]==source:
                counts[sockets[channel]]+=1
        if process.poll() is not None:
            if finished is None: finished=time.monotonic()
            if time.monotonic()-finished>0.3: break
    assert process.returncode==0,process.returncode
    print('Probe-source frame counts by bridge:',counts)
    assert counts[target]>0
    assert all(count==0 for name,count in counts.items() if name!=target)
    drops={name:struct.unpack('II',channel.getsockopt(263,6,8))[1] for channel,name in sockets.items()}
    assert not any(drops.values()),('capture drops; isolation evidence incomplete',drops)
    after=counters()
    delta={key:after[key]-before[key] for key in before}
    print(f'Physical switch port {physical} counter deltas:',delta)
    assert all(delta[key]>0 for key in ('bytes_rx','bytes_tx','packets_rx','packets_tx'))
    assert delta['errs_rx']==delta['errs_tx']==0,delta
    print(f'PASS: {label} frames only on {target}; no cross-bridge/control leakage or capture drops observed')
finally:
    if process is not None and process.poll() is None:
        os.killpg(process.pid,signal.SIGTERM)
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL)
            process.wait()
    for channel in sockets: channel.close()

