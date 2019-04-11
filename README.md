# px4_drv - Unofficial Linux driver for PLEX PX-W3U4/W3PE4/Q3PE4 ISDB-T/S receivers

PLEX PX-W3U4/W3PE4/Q3PE4用の非公式版Linuxドライバです。  
PLEX社の[Webサイト](http://plex-net.co.jp)にて配布されている公式Linuxドライバとは**別物**です。

現在開発中につき、環境によっては動作が安定しない可能性があります。  
予めご了承ください。

## 対応デバイス

- PX-W3U4
- PX-W3PE4
- PX-Q3PE4

このドライバはPX-Q3U4に対応している旨を明記していませんが、恐らく動作します。

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

#### DKMSを使用しない場合

gcc, make, カーネルソース/ヘッダがインストールされている必要があります。

	$ cd driver
	$ make
	$ sudo make install
	$ cd ../

#### DKMSを使用する場合

gcc, make, カーネルソース/ヘッダ, dkmsがインストールされている必要があります。

	$ sudo cp -a ./ /usr/src/px4_drv-0.1.0
	$ sudo dkms add px4_drv/0.1.0
	$ sudo dkms install px4_drv/0.1.0

### 3. 確認

インストールに成功した状態でデバイスが接続されると、`/dev/` 以下に `px4video*` という名前のデバイスファイルが作成されます。

チューナーは、px4video0から ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

## アンインストール

### 1. ドライバのアンインストール

#### DKMSを使用せずにインストールした場合

	$ cd driver
	$ sudo make uninstall
	$ cd ../

#### DKMSを使用してインストールした場合

	$ sudo dkms remove px4_drv/0.1.0 --all
	$ sudo rm -rf /usr/src/px4_drv-0.1.0

### 2. ファームウェアのアンインストール

	$ sudo rm /lib/firmware/it930x-firmware.bin

## 受信方法

recpt1を使用してTSデータの受信ができます。

## LNB電源の出力

出力なしと15Vの出力のみに対応しています。デフォルトではLNB電源の出力を行いません。  
LNB電源の出力を行うには、recpt1を実行する際のパラメータに `--lnb 15` を追加してください。

## 技術情報

### デバイスの構成

PX-W3PE4/Q3PE4は、電源の供給をPCIeスロットから受け、データのやり取りをUSBを介して行います。  
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

### TS Aggregation の設定

sync_byteをデバイス側で書き換え、ホスト側でその値を元にそれぞれのチューナーのTSデータを振り分けるモードを使用しています。
