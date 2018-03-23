#!/bin/bash

cd ../build

# compile

make tokenminer

if [ $? -ge 1 ] ; then
   echo "."
   echo "."
   echo "."
   echo "compilation error"
   echo "."
   echo "."
   echo "."
   exit 1
fi

cp libethcore/libethcore.so ../release/stage/bin
cp libdevcore/libdevcore.so ../release/stage/bin
cp libstratum/libethstratum.so ../release/stage/bin
cp libethash-cl/libethash-cl.so ../release/stage/bin
cp libethash/libethash.so ../release/stage/bin

cp ethminer/tokenminer ../release/stage
cp ../tokenminer.ini ../release/stage
cp ../README.md ../release/stage
cp ../INSTALL-LINUX ../release/stage

cd ../release

rm *.tar.gz
tar -czf mvis-tokenminer-version-linux.tar.gz  --directory=stage .

if [ $? -ge 1 ] ; then
   echo "."
   echo "."
   echo "."
   echo "ERRORS WERE ENCOUNTERED!!!"
   echo "."
   echo "."
   echo "."
   exit 1
fi

echo "."
echo "."
echo "."
echo "script completed successfully"
echo "."
echo "."
echo "."
