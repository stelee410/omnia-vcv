#!/bin/bash

echo "正在执行 make clean..."
make clean
if [ $? -ne 0 ]; then
    echo "错误: make clean 失败"
    exit 1
fi

echo ""
echo "正在执行 make..."
make
if [ $? -ne 0 ]; then
    echo "错误: make 失败"
    exit 1
fi

echo ""
echo "正在执行 make installdev..."
make installdev
if [ $? -ne 0 ]; then
    echo "错误: make installdev 失败"
    exit 1
fi

echo ""
echo "所有步骤完成！"

