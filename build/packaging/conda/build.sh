#!/bin/bash
set -ex

# get the path to the python lib
PYTHON_LIBRARY="PYTHON_LIBRARY_NOT_FOUND"
for i in $PREFIX/lib/libpython${PY_VER}{,m,mu}{.so,.dylib}; do
    if [ -f $i ]; then
        PYTHON_LIBRARY=$i
    fi
done

# create a temporary build directory
BUILD_DIR="CONDOR_BUILD"
mkdir $BUILD_DIR
cd $BUILD_DIR

# cmake from source (up one directory)
cmake .. \
      -DCMAKE_INSTALL_PREFIX:PATH=$PREFIX \
      -DCMAKE_INSTALL_RPATH=$LD_RUN_PATH \
      -DUW_BUILD:BOOL=ON \
      -DCLIPPED:BOOL=ON \
      -DHAVE_BOINC:BOOL=OFF \
      -DENABLE_JAVA_TESTS:BOOL=OFF \
      -DWITH_BLAHP:BOOL=OFF \
      -DWITH_CREAM:BOOL=OFF \
      -DWITH_BOINC:BOOL=OFF \
      -DWANT_PYTHON_WHEELS:BOOL=ON \
      -DPYTHON_INCLUDE_DIR:PATH=$PREFIX/include/python${PY_VER} \
      -DPYTHON_LIBRARY:FILEPATH=$PYTHON_LIBRARY \
      -DPYTHONLIBS_VERSION_STRING:STRING=${PY_VER} \
      -DBUILDID:STRING=UW_Python_Conda_Build

# build targets
make

# put external libraries into path
for extlibdir in $(find $BUILD_DIR/bld_external -name lib -type d); do
    LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$extlibdir
done
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$BUILD_DIR/src/condor_utils
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$BUILD_DIR/src/python-bindings
export LD_LIBRARY_PATH

# install package
cd build/packaging/pypi
python setup.py install

