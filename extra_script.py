import src.bin2c as bin2c
import re
import os
Import("env")  # type: ignore


print("Current CLI targets", COMMAND_LINE_TARGETS)  # type: ignore
print("Current Build targets", BUILD_TARGETS)  # type: ignore


def make_include_h(input, gzip=False, uint16=False):
    # replace everything other than alfanumeric characters with underscore
    varname = re.sub('[^0-9a-zA-Z]+', '_', input)
    output_file = 'src/' + varname + '.h'
    input_file = 'src/' + input
    print('converting ' + input_file + ' to ' + output_file)
    # if output doesn't exist or is older than input
    if not os.path.exists(output_file) or os.path.getmtime(output_file) < os.path.getmtime(input_file):

        with open(output_file, mode='w') as f:
            f.write(bin2c.bin2c(input_file, varname, gzip=gzip, uint16=uint16))


make_include_h('chart-v3.9.1.min.js', gzip=True)
make_include_h('FileSaver-v2.0.5.min.js', gzip=True)
make_include_h('index.html', gzip=True)
make_include_h('nyancat.bmp', uint16=True)
