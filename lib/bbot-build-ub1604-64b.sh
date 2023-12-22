#!/bin/bash

cp depspath.pri.bbot depspath.pri
git clone https://devtools:mkeyDE_BVuETu665SviX@gitlab.netresults.dev:10443/netresults/utils/scripts.git
python3 scripts/wrappergen2/wrappergen.py -c nrpycpp_wrapper_conf.json
qmake-5157
make

