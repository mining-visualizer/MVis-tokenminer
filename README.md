## MVis-tokenminer


This page describes the miner for **0xBitcoin**.  For the regular ethereum miner, [click here](https://github.com/mining-visualizer/MVis-ethminer).

This is a fork of my MVis-ethminer program, which was a fork of Genoil's ethminer-0.9.41-genoil-1.x.x. 

* This miner should work with any GPU that supports OpenCL, ie. pretty much all AMDs and most NVidia.
* Windows binaries can be downloaded from the  [Releases](https://github.com/mining-visualizer/MVis-tokenminer/releases) page, or you can build from source (see below).
* For Linux, the only option at present is to build from source.  See the instructions below.  
* This miner supports both pool mining and solo mining. If you want to mine solo, you either need to run your own node, or use a public one (ie. Infura).
* When in pool mining mode, a 1.0% dev fee is in effect. Every 4 hours it switches to 'dev fee mining' for a short period of time, based on the percent.


### Installation

**YouTube Tutorial** : https://www.youtube.com/watch?v=W-gLERwzKno

* Unzip the [download package](https://github.com/mining-visualizer/MVis-tokenminer/releases) anywhere you like.  
* Double-click on the file `list-devices.bat`.  Examine the screen output and verify your GPU's are recognized.  Pay special attention to the PlatformID.  If it is anything other than 0, you will need to edit the `start-mining.bat` file and change the `--opencl-platform <n>` argument.
* Open up `tokenminer.ini` using any text editor and set the following configuration items, depending on whether you are solo mining or mining on a pool:

##### Pool Mining

* For Pool Mining, you need to specify your mining pool URL and your ETH address. 

```
[Node]
Host=http://your_mining_pool.com:8080

[0xBitcoin]
MinerAcct=0x1234512345123451234512345123451234512345
```
* All other settings in the `[0xBitcoin]` section can be left as is.
* You can also specify the pool mining address on the command line (-N).  See below for all command line options.
* If your mining pool supports the **stratum protocol**, set `Stratum=true` in the Node section, and change the port number to whatever the pool uses.
* Start mining by double-clicking on `start-mining.bat`.

##### Solo Mining

* Specify an ETH account and associated private key in the INI file.
* You need to have access to an Ethereum node: either one you are running locally, or a publicly available node (ie. Infura). You can specify the address and port of your node in the `.ini` file, or on the command line.
* Start mining with `tokenminer.exe -S -G`.  This assumes you've specified the node address in the .INI file.


##### General

* You can leave the .INI file in the executable folder,  or you can move it to `C:\Users\[USER]\AppData\Local\tokenminer` on Windows, or `$HOME/.config/tokenminer` on Linux.  If that folder path does not exist, you will need to create it manually. If for some reason that file exists at both locations, the one in the executable folder will take precedence. 
* **WINDOWS ONLY**: download and install both the [VC 2013 Redistributable](https://www.microsoft.com/en-ca/download/details.aspx?id=40784) and the [VC 2015 Redistributable](https://www.microsoft.com/en-ca/download/details.aspx?id=48145)
* **COOLING**: Please note that MVis-tokenminer does not have any features to set fan speeds or regulate cooling, other than shutting down if things get too hot.  Usually the AMD drivers do a pretty good job in that regard, but sometimes they don't.  It is your responsibility to monitor your fan speeds and GPU temperatures. If the AMD drivers aren't setting fan speeds high enough, you may need to use a 3rd party product,  like Speedfan or Afterburner.

#### Configuration Details ####

MVis-tokenminer is partially configured via command line parameters, and partially by settings in `tokenminer.ini`.  Run `tokenminer --help` to see which settings are available on the command line.  Have a look inside the .ini file to see what settings can be configured there. (It is fairly well commented).  Some settings can *only* be set on the command line (legacy ones mostly), some settings can *only* be set in the .ini file (newer ones mostly), and some can be set in both.  For the last group, command line settings take precedence over the .ini file settings.

#### Command Line Options ####

```
Node configuration:
    -N, --node <host:rpc_port>  Host address and RPC port of your node/mining pool. (default: 127.0.0.1:8545)
    -N2, --node2 <host:rpc_port>  Failover node/mining pool (default: disabled)
    -I, --polling-interval <n>  Check for new work every <n> milliseconds (default: 2000). 
    -R, --farm-retries <n> Number of retries until switch to failover (default: 4)

 Benchmarking mode:
    -M,--benchmark  Benchmark for mining and exit
    --benchmark-warmup <seconds>  Set the duration of warmup for the benchmark tests (default: 8).
    --benchmark-trial <seconds>  Set the duration for each trial for the benchmark tests (default: 3).
    --benchmark-trials <n>  Set the number of benchmark tests (default: 5).

 Mining configuration:
    -P  Pool mining
    -S  Solo mining
    -C,--cpu  CPU mining
    -G,--opencl  When mining use the GPU via OpenCL.
    --cl-local-work <n> Set the OpenCL local work size. Default is 128
    --cl-work-multiplier <n> This value multiplied by the cl-local-work value equals the number of hashes computed per kernel 
       run (ie. global work size). (Default: 8192)
    --opencl-platform <n>  When mining using -G/--opencl use OpenCL platform n (default: 0).
    --opencl-device <n>  When mining using -G/--opencl use OpenCL device n (default: 0).
    --opencl-devices <0 1 ..n> Select which OpenCL devices to mine on. Default is to use all
    -t, --mining-threads <n> Limit number of CPU miners to n (default: use everything available on selected platform)
    --allow-opencl-cpu  Allows CPU to be considered as an OpenCL device if the OpenCL platform supports it.
    --list-devices List the detected OpenCL/CUDA devices and exit. Should be combined with -G or -U flag
    --cl-extragpu-mem <n> Set the memory (in MB) you believe your GPU requires for stuff other than mining. default: 0

 Miscellaneous Options:
    --config <FileSpec>  - Full path to an INI file containing program options. Default location is 1) the executable folder, or 
                           if not there, then in 2) %LocalAppData%/tokenminer/tokenminer.ini (Windows) or 
                           $HOME/.config/tokenminer/tokenminer.ini (Linux).  If this option is specified,  it must appear 
                           before all others.

 General Options:
    -V,--version  Show the version and exit.
    -h,--help  Show this help message and exit.
```

#### INI File Settings

```

[General]

; Optional: uncomment this to specify your own web3 endpoint. you don't need this
; to mine successfully.  it is only used for pool mining, to display your token
; balance on screen. there are a variety of publicly available web3 services. 
; see https://ethereumnodes.com/

; Web3Url=https://mainnet.infura.io/v3/_your_infura_id_
; Web3Url=http://127.0.0.1:8545

;--------------------------------------------------------
[Node]

; If you are pool mining, set Host to the URL of your mining pool.
; If you are solo mining, set the Host to point to your node. Public nodes 
; can also be used (ie. Infura). You can also set these with the -N 
; command line paramater.  The command line overrides settings specified here
;
; Examples, POOL MINING:
;    Host=http://mvis.ca:8080
;
; Examples, SOLO MINING:
;    Host=127.0.0.1:8545
;    Host=https://mainnet.infura.io/your_api_key

Host=

; Pool Mining: Set this to true if the mining pool supports stratum protocol. 
; Currently only mvis.ca does, on port 8090.
Stratum=false

;--------------------------------------------------------
[Node2]

; Secondary (failover) node/mining pool, if you have one. Default is disabled.

; Host=http://your_failover_mining_pool.com

Stratum=false

;--------------------------------------------------------
[0xBitcoin]

; POOL MINING: Your ETH account, to which payouts will be made. THE PRIVATE
;   KEY IS NOT REQUIRED.  Note the acct should start with 0x.
;
; SOLO MINING: Your ETH account and private key.  Note the PK does NOT start
;   with 0x.  Mining rewards will be deposited to this account.  Transaction
;   fees will be DRAWN from this account.  Make sure you have enough funds!!
;   If you have multiple mining rigs, make sure each rig is running under a
;   separate ETH account, to prevent nonce collisions if they happen to submit
;   txs at nearly the same time.

MinerAcct=0x........................................
AcctPK=................................................................

; 0xBitcoin contract address. normally you will not change this.
TokenContract=0xb6ed7644c69416d67b522e20bc294a9a9b405b31

; THE REMAINING SETTINGS IN THIS SECTION APPLY ONLY TO SOLO MINING:

; when your miner finds a solution, transactions will be submitted with this
; amount of gas (gwei).  You can change this setting 'on the fly' (without having
; to restart the miner).  All other setting changes require the miner to be restarted.
GasPrice=5

; Set to 1 to enable gas price bidding.  Transactions will be submitted
; with [GasPrice] gas, unless there is someone else bidding, in which case
; the gas price will be set to the price of the competing bid + [BidTop], up
; to a maximum of [MaxGasPrice]. This feature has only been tested when running
; Geth as a local node.  It is NOT supported by Infura nodes.
GasPriceBidding=0

; max gas price you're willing to bid up to
MaxGasPrice=35

; gas limit used when submitting solution
GasLimit=200000

; the # of gwei to top the highest bidder
BidTop=2

; if you have multiple mining rigs, specify a shared folder to make sure
; multiple miners don't try to submit a solution for the same challenge.
; eg. ChallengeFolder=\\DESKTOP\folder_name
ChallengeFolder=


;--------------------------------------------------------
[ThermalProtection]

; Temperature provider ('amd_adl' or 'speedfan')
TempProvider=amd_adl

; Default temperature at which GPU throttling is activated. This applies to all GPUs on this mining rig.
; Note: throttling is at the hashing level, not the driver level.
ThrottleTemp=80

; Number of seconds after which the entire mining rig will shutdown if one or more GPUs
; remain at or above ThrottleTemp.
ShutDown=20


```

### Building on Windows

- download and install **Visual Studio 2015**.  This project requires the v120 toolset.  See this [Stack Overflow question](https://stackoverflow.com/questions/42669153/using-v120-platform-toolset-in-visual-studio-2015) to make sure the v120 toolset gets installed onto your system.
- download and install **CMake**
- download or clone this repository into a folder. Let's call it `<mvis_folder>`.
- run `getstuff.bat` in the extdep folder.
- open a command prompt in `<mvis_folder>` and enter the following commands.  (Note: the string "*Visual Studio 12 2013 Win64*" is **not** a typo.)

``` 
mkdir build 
cd build
cmake -G "Visual Studio 12 2013 Win64" ..
```

##### Visual Studio

- Use Visual Studio to open `mvis-tokenminer.sln` located in the `build` directory.
- Visual Studio may offer to convert the project files from 2013 format to 2015 format.  **Do not accept this!!**  Click Cancel to keep the projects using the VS2013 toolset.
- Set `tokenminer` as the startup project by right-clicking on it in the project pane.
- Build. Run


### Building on Ubuntu

This has only been verified to work with **Ubuntu 16.04**.  OpenCL only (**for AMD cards**)

```bash
sudo apt-get update
sudo apt-get -y install software-properties-common
sudo add-apt-repository -y ppa:ethereum/ethereum
sudo apt-get update
sudo apt-get install git cmake libcryptopp-dev libleveldb-dev libjsoncpp-dev libjsonrpccpp-dev libboost-all-dev libgmp-dev libreadline-dev libcurl4-gnutls-dev ocl-icd-libopencl1 opencl-headers mesa-common-dev libmicrohttpd-dev build-essential -y
git clone https://github.com/mining-visualizer/MVis-tokenminer.git <mvis_folder>
cd <mvis_folder>
mkdir build
cd build
cmake -DBUNDLE=miner ..
make
```

You can then find the executable in the `build/ethminer` subfolder

##### Ubuntu 18.04

Unverified: A user has reported that you can build this under Ubuntu 18.04 by simply changing `libcryptopp-dev` to `libcrypto++-dev` in the `apt-get install` command above.

### Credits

* LtTofu and other miner software developers on Discord, for their kernel optimizations.



### Donations

Donations can be made to to `mining-visualizer.eth (0xA804e933301AA2C919D3a9834082Cddda877C205)`
