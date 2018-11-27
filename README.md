# Perf_tool
perf_tool是一个数据可视化的web应用性能工具。</br>
有完整的信号，进程，日志管理。</br>
事件驱动，固定多进程数量，无锁高性能。作为一个性能测试工具首先自己的性能的性能不能差。。</br>
前端UI用echarts js实现初略的数据可视化，现在的版本比较粗糙。通过UI设置用例方便快捷，通过UI的图表可以实时，直观的看到数据走向。
![example](https://github.com/hitanxxx/perf_tool/blob/master/www/images/perf_example.png)
# Install
依赖OpenSSL库。解决依赖后。在文件目录运行：
* configure </br>
* make && make install </br>
安装完成后，在/usr/local/lk目录可看到安装完成后的文件。
运行/usr/local/lk/sbin目录下的elf文件即可使用，但是使用之前可能需要了解配置。
> * /usr/local/lk/conf - 配置文件所在目录
> * /usr/local/lk/logs - pid，日志，缓冲文件所在目录
> * /usr/local/lk/sbin - elf执行文件所在目录
> * /usr/local/lk/www  - HTML资源目录

# Command line parameters
* -stop </br>
作用是停止后台所有lk进程。</br>
stop all process when works in the backend
* -reload </br>
作用是重新启动子进程 </br>
reload all worker process

# configuraction
```json
{
	"daemon":true,
	"worker_process":2,

	"reuse_port":false,
	"accept_mutex":false,

	"log_error":true,
	"log_debug":false,

	"sslcrt":"/usr/local/lk/www/certificate/server.crt",
	"sslkey":"/usr/local/lk/www/certificate/server.key",

	"http":{
		"access_log":true,
		"keepalive":false,
		"http_listen":[80],
		"https_listen":[443],
		"home":"/usr/local/lk/www",
		"index":"index.html"
	},
	"perf":{
		"switch":false
	}
}
```
一个典型的配置文件如上，结构划分为多个部分：
* 全局块，一般设置一些共享的信息。
> * daemon - 守护进程开关
> * worker_process - 工作进程数量，为0时，管理进程即为工作进程。
> * reuse_port - socket特性，某些情景优化多进程竞争态。
> * accept_mutex - 信号量锁开关。
> * log_error - error日志开关。
> * log_debug - debug日志开关。（影响性能）
> * sslcrt - SSL证书&公钥。
> * sslkey - SSL私钥。
* http 块，设置一些关于http的信息。
> * access_log - http模块access日志
> * keepalive - http长连接支持开关。
> * http listen - http监听的端口。
> * https listen - https监听的端口。
> * home - HTML资源默认目录。
> * index - HTML资源默认后缀。
* perf 块，指定性能测试模块的信息。
> * switch - 是否开启性能测试模块。如果开启。将只有最后一个启动的工作进程监听。其余工作监听不监听，只用来性能测试。

# Tips
* 通过 /perform.html进入web应用性能测试的UI页面。
* tunnel模块使用的端口为7324，7325。client/proxy使用7325。server使用7324。暂不能通过配置修改。
# Benchmark
## lk-perf VS apachebench
宿主 cpu为 3.8Ghz, 内存ddr4-2400，网络为千兆以太</br>
均为http，connection close短连接。
### 单核心
> * lk-perf 虚拟机 1核心，1gb内存
> * ab 虚拟机 1核心，1gb内存
> * 被测机- nginx 1.14，虚拟机 1核心，1gb内存，1进程。

|    object  |   进程 | 并发设置 | 时间     |  test1    |  test2   |   test3     |  cpu      |
|------------|--------|----------|-----------|-----------|---------|-------------|-----------|
|    lk perf |   1    | 30       |   10s     |70679      |70637    |70580        |   99      |
|    ab      |   /    |  1000    |   10s     |72251      |71711    |72433        |   99      |

### 双核心
> * lk perf 虚拟机 2核心，1gb内存
> * ab虚拟机 2核心，1gb内存
> * 被测试的nginx-1.14，虚拟机 2核心，1gb内存，2进程。

|    object   |   进程  | 并发设置 | 时间      |  test1   |test2     |   test3  |  cpu     |
|-------------|---------|----------|-----------|----------|----------|----------|----------|
|    lk perf  |   2     | 30       |   10s     |194177   |195576     |195954    |  97-96   |
|    ab       |   /     |  3000    |   10s     |181247   |161376     |170303    |   100    |
> *  ab 的test2，test3出现了failed的情况。数量在400左右。
> *  双核心数据和单核心的数据比起来，并不是与单核心的数据x2差不多，而是多了不少。这有点不合逻辑，我不知道为什么。也许是因为虚拟机的关系双核心的情况下占用了比预期更多宿主资源导致的。不过这里想说明的是，对比保持了环境的一致性。数据一定程度上总能体现不同对象在同样环境下的差异。
