"""
HW2 tests.
"""

import subprocess
import os
from os import path


def repair(device):
    return 'REPAIR %d\n' % device


def read(sector):
    return 'READ %d\n' % sector


def write(sector):
    return 'WRITE %d\n' % sector


def kill(device):
    return 'KILL %d\n' % device


test1 = ''
for i in range(0, 500):
    test1 += read(i)


test2 = ''
for i in range(0, 500):
    test2 += write(i)


test3 = ''
test3 += kill(2)
for i in range(0, 500):
    test3 += read(i)
    test3 += write(i)


test4 = ''
test4 += kill(3)
test4 += kill(2)
for i in range(0, 500):
    test4 += read(i)
    test4 += write(i)


test5 = ''
test5 += kill(1)
test5 += kill(3)
test5 += kill(2)
for i in range(0, 500):
    test5 += read(i)
    test5 += write(i)


test6 = ''
test6 += kill(1)
test6 += kill(3)
test6 += kill(2)
for i in range(0, 500):
    test6 += read(i)
    test6 += write(i)
test6 += repair(1)
for i in range(0, 500):
    test6 += read(i)
    test6 += write(i)
test6 += repair(2)
for i in range(0, 500):
    test6 += read(i)
    test6 += write(i)

TESTS = {
    'test1': test1,
    'test2': test2,
    'test3': test3,
    'test4': test4,
    'test5': test5,
    'test6': test6,
 }


def run_test(exe, devices, test_name, test_input):
    p = subprocess.Popen([exe] + devices,
                         stdout=subprocess.PIPE,
                         stdin=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    (stdout, stderr) = p.communicate(test_input)
    res_file = open(path.join(RESULT_DIR, 'result_%s' % test_name), 'w')
    res_file.write('Status: %d\n\n' % p.returncode)
    res_file.write('Stdout:\n')
    res_file.write(stdout)
    res_file.write('\n\nStderr:\n')
    res_file.write(stderr)


def run_all_tests(exe, devices):
    for test_name, test_input in TESTS.items():
        print('running %s' % test_name)
        run_test(exe, devices, test_name, test_input)


EXE = './a.out'
DEVICES = [
    '/dev/sdb',
    '/dev/sdc',
    '/dev/sdd',
    '/dev/sde',
    '/dev/sdf',
]
RESULT_DIR = 'test_results'

def main():
    if not path.exists(RESULT_DIR):
        os.makedirs(RESULT_DIR)
    run_all_tests(EXE, DEVICES)


if __name__ == '__main__':
    main()
