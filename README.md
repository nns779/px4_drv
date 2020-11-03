# px4_drv - Unofficial Linux driver for PLEX PX4/PX-MLT series ISDB-T/S receivers

PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4/MLT5PE/MLT8PE用の非公式版Linuxドライバです。  
PLEX社の[Webサイト](http://plex-net.co.jp)にて配布されている公式Linuxドライバとは**別物**です。

現在開発中につき、環境によっては動作が安定しない可能性があります。  
予めご了承ください。

## 対応デバイス

- PLEX

	- PX-W3U4
	- PX-Q3U4
	- PX-W3PE4
	- PX-Q3PE4
	- PX-MLT5PE
	- PX-MLT8PE

- e-Better

	- DTV02-1T1S-U (実験的)

## 対応予定のデバイス

- PLEX

	- PX-W3PE5

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

##### PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video1  /dev/px4video2  /dev/px4video3

チューナーは、`px4video0`から ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

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

##### e-Better DTV02-1T1S-Uを接続した場合

	$ ls /dev/isdb2056video*
	/dev/isdb2056video0

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

recpt1を使用することで、TSデータの受信を行うことが出来ます。  
recpt1は、PLEX社より配布されているものを使用する必要はありません。

PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4を使用する場合は、recpt1に限らず[BonDriverProxy_Linux](https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux)等のPTシリーズ用chardevドライバに対応したソフトウェアが使用できます。

## LNB電源の出力

### PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4/MLT5PE/MLT8PE

出力なしと15Vの出力のみに対応しています。デフォルトではLNB電源の出力を行いません。  
LNB電源の出力を行うには、recpt1を実行する際のパラメータに `--lnb 15` を追加してください。

### e-Better DTV02-1T1S-U

対応しておりません。

## 備考

### 内蔵カードリーダーについて

このドライバは、各種対応デバイスに内蔵されているカードリーダーの操作には対応していません。  
また、今後対応を行う予定もありません。ご了承ください。

### e-Better DTV02-1T1S-Uについて

e-Better DTV02-1T1S-Uは、個体により正常に動作しないことのある不具合が各所にて多数報告されています。そのため、このドライバでは「実験的な対応」とさせていただいております。  
上記の不具合はこの非公式ドライバでも完全には解消できないと思われますので、その点は予めご了承ください。

## 技術情報

### デバイスの構成

PX-W3PE4/Q3PE4/MLT5PE/MLT8PEは、電源の供給をPCIeスロットから受け、データのやり取りをUSBを介して行います。  
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

PX-MLT8PEは、同一基板上にPX-MLT5PE相当のデバイスと、3チャンネル分のチューナーを持つデバイスが実装されている構造となっています。

- PX-MLT5PE/MLT8PE5

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x5)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x5)

- PX-MLT8PE3

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x3)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x3)

DTV02-1T1S-Uは、ISDB-T側のTSシリアル出力をISDB-S側と共有しています。そのため、同時に受信できるチャンネル数は1チャンネルのみです。

- DTV02-1T1S-U

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90532XBG
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

### TS Aggregation の設定

sync_byteをデバイス側で書き換え、ホスト側でその値を元にそれぞれのチューナーのTSデータを振り分けるモードを使用しています。
