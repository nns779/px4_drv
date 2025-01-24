# px4_drv - Unofficial Linux driver for PLEX PX4/PX5/PX-MLT series ISDB-T/S receivers

PLEXやe-Betterから発売された各種ISDB-T/Sチューナー向けのchardev版非公式Linuxドライバです。  

PLEX社の[Webサイト](http://plex-net.co.jp)にて配布されている公式Linuxドライバとは**別物**です。

## 対応デバイス

- PLEX

	- PX-W3U4
	- PX-Q3U4
	- PX-W3PE4
	- PX-Q3PE4
	- PX-W3PE5
	- PX-Q3PE5
	- PX-MLT5PE
	- PX-MLT8PE
    - PX-M1UR
	- PX-S1UR

- e-Better

	- DTV02-1T1S-U (実験的)
	- DTV02A-1T1S-U
	- DTV02A-4TS-P

## インストール

このドライバを使用する前に、ファームウェアを公式ドライバより抽出しインストールを行う必要があります。

### 1. ファームウェアの抽出とインストール

unzip, gcc, makeがインストールされている必要があります。

	$ cd fwtool
	$ make
	$ wget http://plex-net.co.jp/plex/pxw3u4/pxw3u4_BDA_ver1x64.zip -O pxw3u4_BDA_ver1x64.zip
	$ unzip -oj pxw3u4_BDA_ver1x64.zip pxw3u4_BDA_ver1x64/PXW3U4.sys
	$ ./fwtool PXW3U4.sys it930x-firmware.bin
	$ sudo mkdir -p /lib/firmware
	$ sudo cp it930x-firmware.bin /lib/firmware/
	$ cd ../

### 2. ドライバのインストール

一部のLinuxディストリビューションでは、udevのインストールが別途必要になる場合があります。

#### DKMSを使用しない場合

gcc, make, カーネルソース/ヘッダがインストールされている必要があります。

	$ cd driver
	$ make
	$ sudo make install
	$ cd ../

#### DKMSを使用する場合

gcc, make, カーネルソース/ヘッダ, dkmsがインストールされている必要があります。

	$ sudo cp -a ./ /usr/src/px4_drv-0.2.1
	$ sudo dkms add px4_drv/0.2.1
	$ sudo dkms install px4_drv/0.2.1

### 3. 確認

#### 3.1 カーネルモジュールのロードの確認

下記のコマンドを実行し、`px4_drv`から始まる行が表示されれば、カーネルモジュールが正常にロードされています。

	$ lsmod | grep -e ^px4_drv
	px4_drv                81920  0

何も表示されない場合は、下記のコマンドを実行してから、再度上記のコマンドを実行して確認を行ってください。

	$ modprobe px4_drv

それでもカーネルモジュールが正常にロードされない場合は、インストールから再度やり直してください。

#### 3.2 デバイスファイルの確認

インストールに成功し、カーネルモジュールがロードされた状態でデバイスが接続されると、`/dev/` 以下にデバイスファイルが作成されます。  
下記のようなコマンドで確認できます。

##### PLEX PX-W3U4/W3PE4/W3PE5を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video1  /dev/px4video2  /dev/px4video3

チューナーは、`px4video0`から ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

##### PLEX PX-Q3U4/Q3PE4/Q3PE5を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video2  /dev/px4video4  /dev/px4video6
	/dev/px4video1  /dev/px4video3  /dev/px4video5  /dev/px4video7

チューナーは、`px4video0`から ISDB-S, ISDB-S, ISDB-T, ISDB-T, ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

##### PLEX PX-MLT5PEを接続した場合

	$ ls /dev/pxmlt5video*
	/dev/pxmlt5video0  /dev/pxmlt5video2  /dev/pxmlt5video4
	/dev/pxmlt5video1  /dev/pxmlt5video3

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。

##### PLEX PX-MLT8PEを接続した場合

	$ ls /dev/pxmlt8video*
	/dev/pxmlt8video0  /dev/pxmlt8video3  /dev/pxmlt8video6
	/dev/pxmlt8video1  /dev/pxmlt8video4  /dev/pxmlt8video7
	/dev/pxmlt8video2  /dev/pxmlt8video5

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。

##### PLEX PX-M1URを接続した場合

	$ ls /dev/pxm1urvideo*
	/dev/pxm1urvideo0

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。

##### PLEX PX-S1URを接続した場合

	$ ls /dev/pxs1urvideo*
	/dev/pxs1urvideo0

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。


##### e-Better DTV02-1T1S-U/DTV02A-1T1S-Uを接続した場合

	$ ls /dev/isdb2056video*
	/dev/isdb2056video0

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。

##### e-Better DTV02A-4TS-Pを接続した場合

	$ ls /dev/isdb6014video*
	/dev/isdb6014video0  /dev/isdb6014video2
	/dev/isdb6014video1  /dev/isdb6014video3

すべてのチューナーにおいて、ISDB-TとISDB-Sのどちらも受信可能です。

## アンインストール

### 1. ドライバのアンインストール

#### DKMSを使用せずにインストールした場合

	$ cd driver
	$ sudo make uninstall
	$ cd ../

#### DKMSを使用してインストールした場合

	$ sudo dkms remove px4_drv/0.2.1 --all
	$ sudo rm -rf /usr/src/px4_drv-0.2.1

### 2. ファームウェアのアンインストール

	$ sudo rm /lib/firmware/it930x-firmware.bin

## 受信方法

recpt1や[BonDriverProxy_Linux](https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux)等のPTシリーズ用chardevドライバに対応したソフトウェアを使用することで、TSデータを受信することが可能です。  
recpt1は、PLEX社より配布されているものを使用する必要はありません。

BonDriverProxy_Linuxと、PLEX PX-MLT5PEやe-Better DTV02A-1T1S-Uなどのデバイスファイル1つでISDB-TとISDB-Sのどちらも受信可能なチューナーを組み合わせて使用する場合は、BonDriverとしてBonDriverProxy_Linuxに同梱されているBonDriver_LinuxPTの代わりに、[BonDriver_LinuxPTX](https://github.com/nns779/BonDriver_LinuxPTX)を使用してください。

## LNB電源の出力

### PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4

出力なしと15Vの出力のみに対応しています。デフォルトではLNB電源の出力を行いません。  
LNB電源の出力を行うには、recpt1を実行する際のパラメータに `--lnb 15` を追加してください。

### PLEX PX-W3PE5/Q3PE5

出力なしと15Vの出力のみに対応しているものと思われます。

### PLEX PX-MLT5PE

対応しておりません。

### PLEX PX-MLT8PE

不明です。

### e-Better DTV02-1T1S-U/DTV02A-1T1S-U

対応しておりません。

### e-Better DTV02A-4TS-P

不明です。

## 備考

### 内蔵カードリーダーやリモコンについて

このドライバは、各種対応デバイスに内蔵されているカードリーダーやリモコンの操作には対応していません。  
また、今後対応を行う予定もありません。ご了承ください。

### e-Better DTV02-1T1S-Uについて

e-Better DTV02-1T1S-Uは、個体によりデバイスからの応答が無くなることのある不具合が各所にて多数報告されています。そのため、このドライバでは「実験的な対応」とさせていただいております。  
上記の不具合はこの非公式ドライバでも完全には解消できないと思われますので、その点は予めご了承ください。

### e-Better DTV02A-1T1S-Uについて

e-better DTV02A-1T1S-Uは、DTV02-1T1S-Uに存在した上記の不具合がハードウェアレベルで修正されています。そのため、このドライバでは「正式な対応」とさせていただいております。

## 技術情報

### デバイスの構成

PX-W3PE4/Q3PE4/MLT5PE/MLT8PE, e-Better DTV02A-4TS-Pは、電源の供給をPCIeスロットから受け、データのやり取りをUSBを介して行います。  
PX-W3PE5/Q3PE5は、PX-W3PE4/Q3PE4相当の基板にPCIe→USBブリッジチップを追加し、USBケーブルを不要とした構造となっています。  
PX-Q3U4/Q3PE4は、PX-W3U4/W3PE4相当のデバイスがUSBハブを介して2つぶら下がる構造となっています。

- PX-W3U4/W3PE4

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Toshiba TC90522XBG
	- Terrestrial Tuner: RafaelMicro R850 (x2)
	- Satellite Tuner: RafaelMicro RT710 (x2)

- PX-Q3U4/Q3PE4

	- USB Bridge: ITE IT9305E (x2)
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (x2)
	- Terrestrial Tuner: RafaelMicro R850 (x4)
	- Satellite Tuner: RafaelMicro RT710 (x4)

- PX-W3PE5

	- PCIe to USB Bridge: ASIX MCS9990CV-AA
	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Toshiba TC90522XBG
	- Terrestrial Tuner: RafaelMicro R850 (x2)
	- Satellite Tuner: RafaelMicro RT710 (x2)

- PX-Q3PE5

	- PCIe to USB Bridge: ASIX MCS9990CV-AA
	- USB Bridge: ITE IT9305E (x2)
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (x2)
	- Terrestrial Tuner: RafaelMicro R850 (x4)
	- Satellite Tuner: RafaelMicro RT710 (x4)

PX-MLT8PEは、同一基板上にPX-MLT5PE相当のデバイスと、3チャンネル分のチューナーを持つデバイスが実装されている構造となっています。

- PX-MLT5PE/MLT8PE5

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x5)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x5)

- PX-MLT8PE3

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x3)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x3)

DTV02-1T1S-U/DTV02A-1T1S-Uは、ISDB-T側のTSシリアル出力をISDB-S側と共有しています。そのため、同時に受信できるチャンネル数は1チャンネルのみです。

- DTV02-1T1S-U/DTV02A-1T1S-U

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90532XBG
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

DTV02A-4TS-Pは、PX-MLT5PEから1チャンネル分のチューナーを削減した構造となっています。

- DTV02A-4TS-P

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x4)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x4)

### TS Aggregation の設定

sync_byteをデバイス側で書き換え、ホスト側でその値を元にそれぞれのチューナーのTSデータを振り分けるモードを使用しています。
