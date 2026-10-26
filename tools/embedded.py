#!/usr/bin/env python3
"""Host-visible project, Linux-only embedded tool and device gateway."""
import glob, hashlib, json, os, pathlib, platform, shlex, signal, struct, subprocess, sys, time
CONFIG = {'project': 'leo-radio', 'kind': 'idf', 'default': 'default', 'variants': ['default'], 'chips': {'default': 'esp32c3'}, 'subdirs': {}}
ROOT = pathlib.Path(__file__).resolve().parent.parent
LINUX_PY = sys.executable if os.environ.get('GITHUB_ACTIONS') == 'true' else str(pathlib.Path.home()/'.espressif/python_env/idf5.5_py3.12_env/bin/python')

def linux_path(value):
    if '=' in value and value.startswith('-'):
        key, val = value.split('=', 1)
        return key + '=' + linux_path(val)
    if value.startswith(('/Users/', '/Volumes/', '/System/')):
        return '/mnt/mac' + value
    return value

def variant(env=None):
    env = os.environ if env is None else env
    value = env.get('EMBEDDED_VARIANT') or env.get('RADIO_BOARD') or CONFIG['default']
    project = env.get('RADIO_PROJECT_DIR', '')
    if CONFIG['project'] == 'esp32-s3-companion' and project:
        if project.rstrip('/').endswith('/firmware/manager'): value = 'manager'
        elif not project.rstrip('/').endswith('/firmware/radio'): raise RuntimeError('Unsupported RADIO_PROJECT_DIR; select a known variant')
    if value not in CONFIG['variants']: raise RuntimeError('Unknown variant: ' + value)
    return value

def chip_for(value):
    return CONFIG['chips'].get(value)

def serial_ports():
    found = {}
    for pattern in ['/dev/serial/by-id/*', '/dev/ttyACM*', '/dev/ttyUSB*', '/dev/cu.usb*', '/dev/cu.wchusb*', '/dev/cu.SLAB*']:
        for port in sorted(glob.glob(pattern)):
            found.setdefault(os.path.realpath(port), port)
    return sorted(found.values())

def select_port(explicit='', candidates=None):
    if explicit:
        if not pathlib.Path(explicit).exists() and os.environ.get('EMBEDDED_DRY_RUN') != '1':
            raise RuntimeError('ESP_PORT does not exist in Linux: ' + explicit)
        return explicit
    ports = serial_ports() if candidates is None else candidates
    if not ports: raise RuntimeError('No serial device in Linux. Connect USB and check orb serial list / orb usb list.')
    if len(ports) != 1: raise RuntimeError('Multiple serial devices; set ESP_PORT explicitly: ' + ', '.join(ports))
    return ports[0]

def guard_idf_write(args, env=None):
    env = os.environ if env is None else env
    if any(a in ['flash', 'app-flash', 'bootloader-flash', 'erase-flash', 'encrypted-flash', 'encrypted-app-flash'] for a in args) and env.get('EMBEDDED_FULL_FLASH') != '1':
        raise RuntimeError('Legacy IDF flash may replace boot/OTA metadata. Prefer device-ubuntu.sh flash IMAGE OFFSET; for intentional IDF layout flashing set EMBEDDED_FULL_FLASH=1.')

def build_dir(value):
    key = hashlib.sha256(str(ROOT).encode()).hexdigest()[:10]
    return pathlib.Path.home() / 'build' / CONFIG['project'] / key / value

def run(command, env=None):
    if os.environ.get('EMBEDDED_DRY_RUN') == '1':
        print(json.dumps({'argv': [str(a) for a in command], 'cwd': str(pathlib.Path.cwd())}))
        return 0
    subprocess.run([str(a) for a in command], env=env, check=True)
    return 0

def esp_command(args, value, port=None):
    if not chip_for(value): raise RuntimeError('This variant is not an ESP target')
    command = [sys.executable, '-m', 'esptool', '--chip', chip_for(value)]
    if args not in [['version'], ['--help'], ['-h']]:
        command += ['--port', port or select_port(os.environ.get('ESP_PORT', '')), '--baud', os.environ.get('ESP_BAUD', '115200')]
    return command + args

def valid_chunk(path, record):
    return bool(record and path.is_file() and path.stat().st_size == record.get('bytes') and hashlib.sha256(path.read_bytes()).hexdigest() == record.get('sha256'))

def backup(output, size, value):
    from esptool.targets import CHIP_DEFS
    output = pathlib.Path(output).resolve()
    if output.exists(): raise RuntimeError('Backup output already exists; choose a new filename: ' + str(output))
    if size <= 0 or size % 4096: raise RuntimeError('Backup size must be a positive multiple of 4096')
    port = select_port(os.environ.get('ESP_PORT', ''))
    if os.environ.get('EMBEDDED_DRY_RUN') == '1':
        print(json.dumps({'action': 'backup', 'port': port, 'chip': chip_for(value), 'bytes': size, 'output': str(output)})); return 0
    os.umask(0o077)
    output.parent.mkdir(parents=True, exist_ok=True)
    parts = output.with_name(output.name + '.parts'); parts.mkdir(exist_ok=True)
    manifest = parts / 'manifest.json'
    state = json.loads(manifest.read_text()) if manifest.exists() else {'size': size, 'chip': chip_for(value), 'chunks': {}}
    if state['size'] != size or state['chip'] != chip_for(value): raise RuntimeError('Partial backup belongs to another configuration')
    def save_state():
        temp = manifest.with_suffix('.tmp'); temp.write_text(json.dumps(state, indent=2)); temp.replace(manifest)
    def expired(signum, frame): raise TimeoutError('Chunk transfer timed out')
    old_handler = signal.signal(signal.SIGALRM, expired)
    esp = None
    def connect():
        rom = CHIP_DEFS[chip_for(value)](port, 115200)
        rom.connect()
        stub = rom.run_stub()
        stub.change_baud(int(os.environ.get('ESP_BAUD', '115200')))
        stub.flash_spi_attach(0)
        flash_id = stub.flash_id(); capacity = 1 << ((flash_id >> 16) & 255)
        if size > capacity: stub._port.close(); raise RuntimeError('Requested backup exceeds detected flash capacity')
        identity = bytes(stub.read_mac()).hex()
        if 'device_mac' in state and state['device_mac'] != identity:
            stub._port.close(); raise RuntimeError('Partial backup belongs to another device')
        state['device_mac'] = identity; save_state()
        stub.flash_set_parameters(capacity)
        original = stub.check_command
        def command(*args, **kwargs):
            if len(args) >= 3 and args[1] == stub.ESP_READ_FLASH:
                params = list(struct.unpack('<IIII', args[2])); params[3] = 1
                args = (*args[:2], struct.pack('<IIII', *params), *args[3:])
            return original(*args, **kwargs)
        stub.check_command = command
        return stub
    try:
        # Connect even on resume, so cached chunks cannot be combined with a different device.
        esp = connect()
        for offset in range(0, size, 0x10000):
            length = min(0x10000, size-offset); key = str(offset); path = parts / (hex(offset) + '.bin')
            if valid_chunk(path, state['chunks'].get(key)): continue
            for attempt in range(3):
                try:
                    if esp is None: esp = connect()
                    signal.alarm(30)
                    data = esp.read_flash(offset, length)
                    digest = esp.flash_md5sum(offset, length)
                    if hashlib.md5(data).hexdigest() != digest: raise RuntimeError('Device digest mismatch')
                    signal.alarm(0)
                    path.write_bytes(data)
                    state['chunks'][key] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}; save_state()
                    print('Backup verified:', hex(offset + length), '/', hex(size), flush=True)
                    break
                except Exception:
                    signal.alarm(0)
                    if esp is not None: esp._port.close()
                    esp = None
                    if attempt == 2: raise
                    time.sleep(1)
        data = b''.join((parts/(hex(offset)+'.bin')).read_bytes() for offset in range(0,size,0x10000))
        if hashlib.md5(data).hexdigest() != esp.flash_md5sum(0,size): raise RuntimeError('Full backup device digest mismatch')
        temp = output.with_name(output.name + '.partial'); temp.write_bytes(data); temp.replace(output)
        state['complete_sha256'] = hashlib.sha256(data).hexdigest(); save_state()
        output.with_name(output.name+'.sha256').write_text(state['complete_sha256']+'  '+output.name+'\n')
        print('Backup complete:', output)
        esp.hard_reset()
    finally:
        signal.alarm(0); signal.signal(signal.SIGALRM, old_handler)
        if esp is not None: esp._port.close()
    return 0

def idf(args, value):
    if CONFIG['kind'] != 'idf': raise RuntimeError('Not an ESP-IDF project')
    guard_idf_write(args)
    # GitHub IDF container already activates its own pinned Linux SDK/Python.
    # Keep the repository build/ expected by existing release workflows.
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        sdk = pathlib.Path(os.environ.get('IDF_PATH', ''))
        if not (sdk/'tools/idf.py').is_file(): raise RuntimeError('GitHub IDF CI requires an activated IDF_PATH')
        directory = ROOT / CONFIG.get('subdirs', {}).get(value, '.') / 'build'
        if list(args) == ['where']: print(directory); return 0
        return run([sys.executable, sdk/'tools/idf.py', '-B', directory, *args])
    args = list(args); port = os.environ.get('ESP_PORT', ''); baud = os.environ.get('ESP_BAUD', '115200')
    for short, long, field in [('-p','--port','port'),('-b','--baud','baud')]:
        for flag in [short,long]:
            if flag in args:
                index=args.index(flag)
                if index+1 >= len(args): raise RuntimeError('Missing argument for '+flag)
                if field=='port': port=args[index+1]
                else: baud=args[index+1]
                del args[index:index+2]
    if any(a in ['-B','--build-dir','-C','--project-dir'] for a in args): raise RuntimeError('Use EMBEDDED_VARIANT instead of overriding IDF build/source paths')
    directory = build_dir(value)
    if args == ['where']: print(directory); return 0
    if 'build' in args:
        extra = [a for a in args if a not in ['build','-D','RADIO_UI_TEST=OFF','monitor']]
        if extra: raise RuntimeError('Unsupported build flags; use build-ubuntu.sh or update its declared variant: '+str(extra))
        build_env=os.environ.copy()
        if 'RADIO_UI_TEST=OFF' in args:build_env['RADIO_UI_TEST']='OFF'
        run(['bash', ROOT/'tools/build-ubuntu.sh', value],env=build_env)
        if 'monitor' not in args: return 0
        return monitor(port, baud)
    if args == ['monitor']: return monitor(port, baud)
    sdk = pathlib.Path.home()/'esp/esp-idf'; env=os.environ.copy()
    env.update(IDF_PATH=str(sdk),IDF_TOOLS_PATH=str(pathlib.Path.home()/'.espressif'),IDF_PYTHON_ENV_PATH=str(pathlib.Path(sys.executable).parent.parent),UBUNTU_DEPENDENCIES_LOCK=str(directory/'dependencies.lock'))
    if not (directory/'CMakeCache.txt').exists() and args not in [['--version'],['--help'],['-h']]: run(['bash',ROOT/'tools/build-ubuntu.sh',value])
    subdir = CONFIG.get('subdirs',{}).get(value,'.'); os.chdir(ROOT/subdir)
    command=[sys.executable,sdk/'tools/idf.py','-B',directory,'-D','SDKCONFIG='+str(directory/'sdkconfig')]
    if any(a in ['flash','app-flash','bootloader-flash','erase-flash','encrypted-flash','encrypted-app-flash'] for a in args): command+=['-p',select_port(port),'-b',baud]
    command += args
    return run(['bash','-c','set -e; source "$IDF_PATH/export.sh" >/dev/null; exec "$@"','embedded-idf',*command],env)

def monitor(port='', baud='115200'):
    return run([sys.executable,'-m','serial.tools.miniterm','--raw','--exit-char','29',select_port(port or os.environ.get('ESP_PORT','')),baud])

def main(args=None):
    args=list(sys.argv[1:] if args is None else args)
    if not args or args[0] in ['--help','-h']:
        print('Usage: tools/device-ubuntu.sh ports|version|info|backup FILE SIZE|flash IMAGE OFFSET|verify IMAGE OFFSET|monitor|esptool ARGS|idf ARGS|pio ARGS|serialtool ARGS|python ARGS|script FILE ARGS|where')
        print('Tools run in embedded/johnson; ESP_PORT and ESP_BAUD refer to Linux. Multiple USB devices require ESP_PORT. Flash addresses must be explicit.');return 0
    known={'ports','version','info','backup','flash','verify','monitor','esptool','idf','pio','serialtool','python','script','where'}
    if args[0] not in known: raise RuntimeError('Unknown action: '+args[0])
    value=variant()
    if platform.system()=='Darwin':
        forwarded={k:linux_path(v) for k,v in os.environ.items() if k in ['ESP_PORT','ESP_BAUD','EMBEDDED_VARIANT','RADIO_BOARD','RADIO_PROJECT_DIR','MANAGER_PROJECT_DIR','BUILD_JOBS','DIST_DIR','EMBEDDED_FULL_FLASH','EMBEDDED_DRY_RUN','MONITOR_BAUD','RADIO_UI_TEST']}
        def host_argument(arg):
            if arg.startswith(('/tmp/','/private/')): return '/mnt/mac'+str(pathlib.Path(arg).resolve())
            if not arg.startswith(('/', '-')) and ('/' in arg or pathlib.Path(arg).suffix in {'.bin','.elf','.json','.csv','.txt','.py','.sh','.zip'}):
                return linux_path(str(pathlib.Path(arg).resolve()))
            return linux_path(arg)
        if args[0] in ['backup','flash','verify'] and len(args)>1:
            args[1]=linux_path(str(pathlib.Path(args[1]).resolve()))
        cmd=['orb','run','-m',os.environ.get('EMBEDDED_MACHINE','embedded'),'-u',os.environ.get('EMBEDDED_USER','johnson'),'env',*['%s=%s'%p for p in forwarded.items()],'python3',linux_path(str(ROOT/'tools/embedded.py')),*[host_argument(a) for a in args]]
        os.execvp(cmd[0],cmd)
    if platform.system()!='Linux': raise RuntimeError('Requires macOS + OrbStack or Linux')
    if pathlib.Path(sys.executable).absolute()!=pathlib.Path(LINUX_PY):
        os.execv(LINUX_PY,[LINUX_PY,str(ROOT/'tools/embedded.py'),*args])
    os.chdir(ROOT)
    action,*args=args; args=[linux_path(a) for a in args]
    if action=='ports': print('\n'.join(serial_ports()));return 0
    if action=='where': print(build_dir(value));return 0
    if action=='version': return run([sys.executable,'-m','esptool','version'])
    if action=='info': return run(esp_command(['flash_id'],value))
    if action=='esptool': return run(esp_command(args,value))
    if action=='monitor': return monitor(baud=os.environ.get('MONITOR_BAUD','115200'))
    if action=='backup':
        if len(args)!=2: raise RuntimeError('backup requires FILE SIZE')
        return backup(args[0],int(args[1],0),value)
    if action in ['flash','verify']:
        if len(args)!=2: raise RuntimeError(action+' requires IMAGE OFFSET')
        image=pathlib.Path(args[0]);address=int(args[1],0)
        if not image.is_file() or address<0 or address%4096: raise RuntimeError('Missing image or invalid flash offset')
        return run(esp_command(['write_flash' if action=='flash' else 'verify_flash',hex(address),str(image)],value))
    if action=='idf': return idf(args,value)
    if action=='pio':
        if CONFIG['kind']!='pio': raise RuntimeError('Not a PlatformIO project')
        environments=[]
        for i,arg in enumerate(args):
            if arg in ['-e','--environment']:
                if i+1>=len(args):raise RuntimeError('Missing PlatformIO environment')
                environments.append(args[i+1])
            elif arg.startswith('--environment='):environments.append(arg.split('=',1)[1])
        if len(environments)>1 or any(',' in e for e in environments):raise RuntimeError('Use one environment per invocation')
        if environments:
            selected='test-native' if environments[0]=='native' else environments[0]
            value=variant({'EMBEDDED_VARIANT':selected})
        elif args and args[0] in ['run','test','debug']:
            args+=['--environment','native' if value=='test-native' else value]
        if value=='test-native' and any(a in ['upload','uploadfs'] for a in args):raise RuntimeError('Native tests cannot be uploaded')
        if args[:2]==['device','monitor'] and '--port' not in args:args+=['--port',select_port(os.environ.get('ESP_PORT','')),'--baud',os.environ.get('MONITOR_BAUD','115200')]
        env=os.environ.copy();env['PLATFORMIO_CORE_DIR']=str(pathlib.Path.home()/('.platformio-legacy' if CONFIG['project']=='esp32-lcd-test' and not value.startswith('c6-') else '.platformio'));env['PLATFORMIO_BUILD_DIR']=str(build_dir(value));env['PLATFORMIO_LIBDEPS_DIR']=str(pathlib.Path.home()/'.cache/platformio-libdeps'/CONFIG['project']/hashlib.sha256(str(ROOT).encode()).hexdigest()[:10])
        if any(a in ['upload','uploadfs'] for a in args) and '--upload-port' not in args: args+=['--upload-port',select_port(os.environ.get('ESP_PORT',''))]
        return run([pathlib.Path.home()/'.venvs/platformio/bin/pio',*args],env)
    if action=='serialtool':
        if CONFIG['kind']!='arm': raise RuntimeError('serialtool is only for UV-K1')
        if not any(a in ['--help','-h','decode'] for a in args) and not any(a=='--port' or a=='-p' or a.startswith('--port=') for a in args): args=[args[0],'--port',select_port(os.environ.get('ESP_PORT','')),*args[1:]]
        return run([sys.executable,ROOT/'tools/serialtool/cli.py',*args])
    if action=='python': return run([sys.executable,*args])
    if action=='script':
        if not args: raise RuntimeError('script requires FILE')
        env=os.environ.copy();env['PATH']=str(pathlib.Path(sys.executable).parent)+':'+env['PATH']
        return run(['bash',*args],env)
    raise RuntimeError('Unsupported action')

if __name__=='__main__':
    try:sys.exit(main())
    except (RuntimeError,ValueError,OSError) as error:print('Error:',error,file=sys.stderr);sys.exit(2)
    except subprocess.CalledProcessError as error:sys.exit(error.returncode)
