# README

* 需要安装的lib
  * [curl](https://github.com/bagder/curl)
     - 安装curl可能需要安装
     - autoconf (sudo apt-get install autoconf)
     - libtool (sudo apt-get install libtool)
  * [liburi](https://github.com/mitghi/liburi)
  * [libuv](https://github.com/libuv/libuv)
  * [libxml2](http://xmlsoft.org/index.html)
     - 安装libxml可能需要安装
     - python-dev (sudo apt-get install python-dev)
  * [pcre](http://www.pcre.org)
  * [CSpider](https://github.com/xonce/CSpider)
     * 编译时可能需要将libxml2的库复制到libxml文件夹下
     * sudo cp -rf /usr/local/include/libxml2/libxml/ /usr/local/include/libxml
  * [libfuse](https://github.com/libfuse/libfuse)
* 安装中出现问题可以尝试

```
sudo apt-get install libcpptest-dev
sudo apt-get install doxygen
sudo apt-get install graphviz
```
