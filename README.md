# px4_drv - Unofficial Linux / Windows (WinUSB) driver for PLEX PX4/PX5/PX-MLT series / e-Better DTV series ISDB-T/S receivers

PLEX や e-Better から発売された各種 ISDB-T/S チューナー向けの chardev 版非公式 Linux ドライバ / Windows (WinUSB) ドライバです。  
PLEX 社の [Webサイト](http://plex-net.co.jp) にて配布されている公式ドライバとは**別物**です。

## このフォークについて

本家 [nns779/px4_drv](https://github.com/nns779/px4_drv) は 2021 年以降メンテナンスされておらず、残念ながら [nns779](https://github.com/nns779) 氏自身もネット上から失踪されています。  
このフォークは、各々のフォークに散逸していた有用な変更を一つにまとめ、継続的にメンテナンスしていくことを目的としています。

### 変更点 (WinUSB 版)

動作確認は Windows 10 / Windows 11 (x64) で行っています。

- エラー発生時の MessageBox を表示しない設定を追加 
  - BonDriver の ini 内の `DisplayErrorMessage` を 1 に設定すると今まで通り MessageBox が表示される
- BS/CS の ChSet に2024年10月～2025年1月に行われた BS トランスポンダ再編後の物理チャンネル情報を反映
- [hendecarows 氏のフォーク](https://github.com/hendecarows/px4_drv) での更新を取り込み、DTV02A-1T1S-U / DTV03A-1TU / PX-M1UR / PX-S1UR に対応
- PX-Q3PE5 に対応し、inf ファイルを追加
- PX-MLT5PE のリブランド品である DTV02A-5TS-P (USB Product ID: 0x924e) に対応し、inf ファイルを追加 (v0.6.1 以降)
- inf ファイルをより分かりやすい名前に変更
- inf ファイルを ARM 版 Windows でもインストールできるようにする
  - 実機がないので試せていないけど、おそらくインストールできるはず
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1625673548/762
- inf ファイルを改善し、チューナーをタスクトレイの「ハードウェアの安全な取り外し」に表示しないようにする
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1666581918/207
- inf ファイルに自己署名を行い、自己署名証明書のインストールだけで正式なドライバーと認識されるように
  - 以前は署名がないためインストール時にドライバー署名の強制の無効化が必要だったが、事前に自己署名証明書を適切な証明書ストアにインストールしておけばこの作業が不要になる
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1577466040/104-108
- 自己署名証明書のインストール・アンインストールスクリプトを追加
  - 拡張子が .jse となっているが、これは PowerShell スクリプトにダブルクリックで実行させるための JScript コードを先頭の行に加えたもの
- 地上波の ChSet に物理 53ch ～ 62ch の定義を追加
  - 物理 53ch ～ 62ch は地上波の割り当て周波数から削除されているが、現在も ”イッツコムch10” など、一部ケーブルテレビの自主放送の割り当て周波数として使われている
- BS/CS の ChSet に2022年3月開局の BS 新チャンネル（BS松竹東急・BSJapanext・BSよしもと）の定義を追加
- バージョン情報が DLL のプロパティに表示されないのを修正
- ビルドとパッケージングを全自動で行うスクリプトを追加
  - Visual Studio 2022 が入っていれば、build.ps1 を実行するだけで全自動でビルドからパッケージングまで行える
- 各機種の内蔵 B-CAS カードリーダーを利用するためのドライバを追加 (v0.6.0 以降)
  - 同梱の `WinSCard.dll` を利用するアプリケーションの実行ファイルと同じフォルダに配置すると、内蔵カードリーダーを優先して利用できるようになる
  - Windows が認識している外付けカードリーダーも引き続き利用できる
- チャンネル切り替え、USB の切断・再接続、内蔵カードリーダーとの同時利用に関する安定性を改善 (v0.6.0 以降)
- fwtool: PX-W3PE5 / PX-Q3PE4 / PX-Q3PE5 / PX-W3U4 / PX-Q3U4 / PX-MLT5PE / PX-MLT8PE の2021年版公式 Windows ドライバからのファームウェア抽出に対応
  - 既定の抽出済みファームウェアで問題なく動作することが知られているため、特段実用性はない
- README（このページ）に WinUSB 版のインストール方法などを追記

### 変更点 (Linux 版)

動作確認は Ubuntu 22.04 LTS (x64) で行っています。

- チップ構成が一部変更された、ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U に対応
- [otya 氏のフォーク](https://github.com/otya128/px4_drv) での更新を取り込み、安定性と互換性を改善
- [techmadot 氏のフォーク](https://github.com/techmadot/px4_drv) の更新を取り込み、PX-M1UR / PX-S1UR に対応
- [kznrluk 氏のフォーク](https://github.com/kznrluk/px4_drv) の更新を取り込み、Linux カーネル 6.4 系以降の API 変更に対応
- [hendecarows 氏のフォーク](https://github.com/hendecarows/px4_drv) での更新を取り込み、DTV03A-1TU に対応
- PX-MLT5PE のリブランド品である DTV02A-5TS-P (USB Product ID: 0x924e) に対応
- WinUSB 版と同じく、PX-S1UR を ISDB2056 系の共通処理に統合し、設計の異なる DTV03A-1TU (ISDBT2071) を単独の実装に分離
- Linux 4.15 (Ubuntu 18.04 の初期カーネル) 以降でビルドできるように修正
- https://github.com/tsukumijima/px4_drv/pull/33 をマージし、IBT (Indirect Branch Tracking) 対応に伴う `objtool` の検査強化により、Linux 6.15 系などでモジュールをビルド・ロードできない問題に対応
  - 当初は保守的に Linux 6.15.4 以降でのみ `module_init()` と `module_exit()` を使っていたが、これらは古いカーネルでも利用できるため、v0.6.0 以降ではカーネル版による分岐を削除して統一
- https://github.com/tsukumijima/px4_drv/pull/6 をマージし、Linux カーネル 6.8 系以降の API 変更に対応
- https://github.com/tsukumijima/px4_drv/pull/3 をマージし、`ctrl_timeout` をモジュールパラメーターに追加
- Debian パッケージ (.deb) の作成とインストールに対応
- DKMS でのインストール時にファームウェアを自動でインストールするように変更
- README (このページ) に Debian パッケージからのインストール方法などを追記

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
	  - チップ構成が一部変更された、ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U にも対応しています。  
	  手元の実機では問題なく動作していますが、長期間の動作テストは行えていないため、未知の不具合があるかもしれません。
	- DTV02A-4TS-P
	- DTV02A-5TS-P (PX-MLT5PE のリブランド品)
	- DTV03A-1TU (実験的)
	  - チップ構成が大幅に変更された、ロット番号 2021-11 以降の個体のみ対応しています。

> [!NOTE]
> 2021 年以降メンテナンスされていない [nns779/px4_drv](https://github.com/nns779/px4_drv) と異なり、新規に下記チューナーのサポートを追加しています。
> 
> - PLEX PX-M1UR
> - PLEX PX-S1UR
> - e-Better DTV02A-5TS-P (PX-MLT5PE のリブランド品)
> - e-Better DTV02A-1T1S-U / Digibest ISDB2056 (Windows 版ドライバを新規追加)
> - e-Better DTV02A-1T1S-U (ロット番号 2309 以降) / Digibest ISDB2056N
> - e-Better DTV03A-1TU / Digibest ISDBT2071 (ロット番号 2021-11 以降)

> [!WARNING]
> PX-M1UR または DTV02(A)-1T1S-U で CATV（周波数変換パススルー）の ISDB-T C13ch ~ C24ch を受信するには、[専用の recpt1 フォーク (hendecarows/recpt1)](https://github.com/hendecarows/recpt1) が必要となります。  
> [stz2012/recpt1](https://github.com/stz2012/recpt1) や [recisdb](https://github.com/kazuki0824/recisdb-rs) では、当該機種にて C13ch ~ C62ch を正常に選局できないことが報告されています（[詳細はこちら](https://github.com/tsukumijima/px4_drv/issues/16)）。  
> ただし、C13ch ~ C24ch 以外のチャンネルであれば、stz2012/recpt1 や recisdb でも問題なく選局・受信が可能です。

## インストール (Windows)

Windows (WinUSB) 版のドライバは、OS にチューナーを認識させるための inf ファイルと、px4_drv 専用の BonDriver、ドライバの実体でチューナー操作を司る DriverHost_PX4 から構成されています。

**ビルド済みのアーカイブは [tsukumijima/DTV-Builds](https://github.com/tsukumijima/DTV-Builds) からダウンロードできます。**  
または、winusb フォルダにある build.ps1 を実行して、ご自分でビルドしたものを使うこともできます (Visual Studio 2022 が必要です) 。

### 1. 自己署名証明書のインストール

Driver フォルダには、各機種ごとのドライバのインストールファイル (.inf) が配置されています。  
ドライバをインストールする前に cert-install.jse を実行し、自己署名証明書をインストールしてください。

自己署名証明書をインストールして信頼することで、自己署名証明書を使用して署名されたドライバも、（自己署名証明書をアンインストールするまでは）通常の署名付きドライバと同様に信頼されるようになります。

cert-install.jse が Smart App Control や関連付けの変更などにより実行できない場合は、管理者権限の Windows Terminal または PowerShell を開き、ビルド済みアーカイブ内の Driver フォルダに移動した上で、下記コマンドを実行してください。

```powershell
certutil.exe -addstore Root ".\px4_drv_winusb.cer"
certutil.exe -addstore TrustedPublisher ".\px4_drv_winusb.cer"
```

### 2. ドライバのインストール

自己署名証明書をインストールしたあとは、公式ドライバと同様の手順でインストールできると思います（公式ドライバは事前にアンインストールしてください）。

具体的には、デバイスマネージャーからチューナーデバイスを選択し、右クリックメニューの [ドライバーの更新] → [コンピューターを参照してドライバーを検索] から、inf ファイルが入っている Driver フォルダを指定してください。[ドライバーが正常に更新されました] と表示されていれば OK です。

### 3. BonDriver の配置

チューナーの機種に応じた BonDriver を配置します。

- PX4/PX5 シリーズの機種: `BonDriver_PX4`
- PX-MLT シリーズの機種・DTV02A-4TS-P・DTV02A-5TS-P: `BonDriver_PX-MLT`
- DTV02A-1T1S-U: `BonDriver_ISDB2056`
- DTV02A-1T1S-U (ロット番号 2309 以降): `BonDriver_ISDB2056N`
- DTV03A-1TU: `BonDriver_ISDBT2071`
- PX-M1UR: `BonDriver_PX-M1UR`
- PX-S1UR: `BonDriver_PX-S1UR`

お使いの PC に合うビット数のフォルダの中のファイルを**すべて**選択し、TVTest や EDCB などのソフトウェアが指定する BonDriver の配置フォルダにコピーします。  
BonDriver と同じフォルダに DriverHost_PX4.exe / DriverHost_PX4.ini / it930x-firmware.bin があることを確認してください。

使用にあたり、特段 ini ファイルの設定変更などは必要ありません。ソフトウェアごとにチャンネルスキャンを行えばそのまま視聴できます。  
なお、BonDriver の ini ファイル内の `DisplayErrorMessage` を 1 に設定すると、オリジナル同様にエラー発生時にメッセージボックスを表示します。

### 4. 内蔵 B-CAS カードリーダーの利用

**v0.6.0 以降、winUSB 版 px4_drv 専用の WinSCard.dll を TVTest.exe や EpgDataCap_Bon.exe と同じフォルダに配置することで、内蔵カードリーダーが利用できるようになりました！**

機種別の BonDriver フォルダに同梱されている `WinSCard.dll` を、TVTest や EDCB など、B-CAS カードを利用するアプリケーションの実行ファイルと同じフォルダにコピーしてください。

> [!NOTE]
> **Windows が認識している外付けカードリーダーと内蔵カードリーダーは併用できます。**  
> カードリーダーの一覧には内蔵カードリーダー、外付けカードリーダーの順で表示されます。  

> [!NOTE]
> **リモートデスクトップ接続中に、チューナーを接続した PC 側の外付けカードリーダーが見つからなくなる場合があります。**  
> RDP のスマートカードリダイレクトが有効なセッションでは、Windows 標準の WinSCard API が接続元 PC 側のカードリーダーを参照するためです。  
> [Windows 標準の対処方法](https://learn.microsoft.com/ja-jp/azure/virtual-desktop/redirection-configure-smart-cards)として、リモート デスクトップ接続の「オプションの表示」→「ローカル リソース」→「詳細」からスマートカードのチェックを外すか、接続先 PC の `gpedit.msc` で「コンピューターの構成」→「管理用テンプレート」→「Windows コンポーネント」→「リモート デスクトップ サービス」→「リモート デスクトップ セッション ホスト」→「デバイスとリソースのリダイレクト」→「スマート カード デバイスのリダイレクトを許可しない」を有効にしてください。  
> 内蔵カードリーダーへの通信は、`DriverHost_PX4.exe` が接続先 PC の WinUSB デバイスと直接行うため、RDP のスマートカードリダイレクトを経由しません。  
> ただし、`WinSCard.dll` を配置していても、外付けカードリーダーを選んだ処理は Windows 標準の WinSCard API へ引き渡されます。この場合は RDP の設定に従い、接続元 PC 側のカードリーダーだけが外付けカードリーダーとして表示されることがあります。

> [!WARNING]  
> **アプリケーション側のビット数 (32bit/64bit) と `WinSCard.dll` のビット数を合わせる必要があります。**  
> また、すでに別実装の `WinSCard.dll` を配置している場合は、元の DLL を退避してから入れ替えてください。  
> 同じフォルダで2つの `WinSCard.dll` を併用することはできません。

> [!TIP]
> `WinSCard.dll` の追加・退避後、TVTest ではカードリーダーエラーが発生することがありますが、これは過去使っていたカードリーダーにアクセスできなくなったことによるエラーですので、異常ではありません。  
> **「設定」→「TSプロセッサー」→「デフォルトフィルター」のセレクトボックスで利用するカードリーダー名を選択し、TVTest を再起動すると、そのカードリーダーに挿入された B-CAS カードを使ってスクランブル解除を行えます。**  

> [!TIP]
> **PC に複数のチューナーを接続している場合、TVTest では複数の内蔵カードリーダーを認識します。** B-CAS カードを挿入したチューナーの内蔵カードリーダーを選択してください。  
> **内蔵カードリーダーを使うために、PC に接続されているすべてのチューナーに B-CAS カードを挿入する必要はありません。** 内蔵カードリーダーは、受信中チューナーと異なるデバイスでも利用できます。

> [!NOTE]  
> `WinSCard.dll` は必要に応じて同じフォルダの `DriverHost_PX4.exe` を起動し、内蔵カードリーダーとの通信を仲介します。  
> 複数の TVTest や EDCB プロセスが同じカードへ接続した場合も、APDU と PC/SC トランザクションはカード単位で直列に処理されます。

## インストール (Linux)

### 1. ファームウェアの抽出とインストール (手動インストール時のみ)

> [!IMPORTANT]  
> **px4_drv を Debian パッケージや DKMS を使用してインストールする際、ファームウェアは自動的にインストールされます。**  
> **Debian パッケージや DKMS を使用してインストールを行う場合は、この手順をスキップしてください。**

事前に unzip, gcc, make がインストールされている必要があります。

	$ cd fwtool
	$ make
	$ wget http://plex-net.co.jp/plex/pxw3u4/pxw3u4_BDA_ver1x64.zip -O pxw3u4_BDA_ver1x64.zip
	$ unzip -oj pxw3u4_BDA_ver1x64.zip pxw3u4_BDA_ver1x64/PXW3U4.sys && rm pxw3u4_BDA_ver1x64.zip
	$ ./fwtool PXW3U4.sys it930x-firmware.bin && rm PXW3U4.sys
	$ sudo mkdir -p /lib/firmware && sudo cp it930x-firmware.bin /lib/firmware/
	$ cd ../

または、抽出済みのファームウェアを利用することもできます。

	$ sudo mkdir -p /lib/firmware && sudo cp ./etc/it930x-firmware.bin /lib/firmware/

### 2. ドライバのインストール

px4_drv は、Linux 4.15 (Ubuntu 18.04 の初期カーネル) 以降のカーネルでビルドできます。  
一部の Linux ディストリビューションでは、別途 udev のインストールが必要になる場合があります。
	
#### Debian パッケージを使用してインストール (強く推奨)

Debian パッケージを使用してインストールすると依存パッケージも自動インストールされるほか、DKMS のソースコード管理も透過的に行われます。  
Ubuntu / Debian 環境では Debian パッケージを使用してインストールすることを強く推奨します。

	$ wget https://github.com/tsukumijima/px4_drv/releases/download/v0.6.1/px4-drv-dkms_0.6.1_all.deb
	$ sudo apt install -y ./px4-drv-dkms_0.6.1_all.deb

上記コマンドで、px4_drv の Debian パッケージをインストールできます。

> [!TIP]
手動で Debian パッケージを生成することもできます。  
> `./build_deb.sh` を実行すると、`./build_deb.sh` の一つ上層のディレクトリに `px4-drv-dkms_0.6.1_all.deb` という名前の Debian パッケージが生成されます。  
> ```
> $ ./build_deb.sh
> $ sudo apt install -y ../px4-drv-dkms_0.6.1_all.deb
> ```
> 上記コマンドで、生成した px4_drv の Debian パッケージをインストールできます。

#### DKMS を使用してインストールする

gcc, make, カーネルソース/ヘッダ, dkms がインストールされている必要があります。

	$ sudo cp -a ./ /usr/src/px4_drv-0.6.1
	$ sudo dkms add px4_drv/0.6.1
	$ sudo dkms install px4_drv/0.6.1

#### DKMS を使用せずにインストールする

事前に gcc, make, カーネルソース/ヘッダがインストールされている必要があります。

> [!WARNING]  
> DKMS を使用せずにインストールした場合、**カーネルのアップデート時にドライバが自動で再ビルドされないため、アップデート後に再度インストールを行う必要があります。**  
> **可能な限り DKMS を使用してインストールすることをおすすめします。**

	$ cd driver
	$ make
	$ sudo make install
	$ cd ../

### 3. 確認

#### 3.1 カーネルモジュールのロードの確認

**下記のコマンドを実行し、`px4_drv` から始まる行が表示されれば、カーネルモジュールが正常にロードされています。**

	$ lsmod | grep -e ^px4_drv
	px4_drv                81920  0

**何も表示されない場合は、下記のコマンドでカーネルモジュールを明示的にロードしてから、再度上記のコマンドを実行して確認を行ってください。**

	$ sudo modprobe px4_drv

それでもカーネルモジュールが正常にロードされない場合は、インストールから再度やり直してください。

> [!IMPORTANT]  
> **px4_drv や Linux カーネルの更新後、px4_drv は自動再ロードされない場合があります。**  
> **px4_drv や Linux カーネルを更新した際は、可能な限りすぐに PC を再起動することを強く推奨します。**   
> すぐに再起動ができない状況では、必ず `sudo modprobe px4_drv` を実行してください。  
> (当然ですが、録画予約のある時間帯に px4_drv や Linux カーネルを更新するべきではありません。)

#### 3.2 デバイスファイルの確認

インストールに成功し、カーネルモジュールがロードされた状態でデバイスが接続されると、`/dev/` 以下にデバイスファイルが作成されます。  
下記のようなコマンドで確認できます。

##### PLEX PX-W3U4/W3PE4/W3PE5 を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video1  /dev/px4video2  /dev/px4video3

チューナーは、`px4video0` から ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

##### PLEX PX-Q3U4/Q3PE4/Q3PE5 を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video2  /dev/px4video4  /dev/px4video6
	/dev/px4video1  /dev/px4video3  /dev/px4video5  /dev/px4video7

チューナーは、`px4video0` から ISDB-S, ISDB-S, ISDB-T, ISDB-T, ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、S と T が2つずつ交互に割り当てられます。

##### PLEX PX-MLT5PE / e-Better DTV02A-5TS-P を接続した場合

	$ ls /dev/pxmlt5video*
	/dev/pxmlt5video0  /dev/pxmlt5video2  /dev/pxmlt5video4
	/dev/pxmlt5video1  /dev/pxmlt5video3

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-MLT8PE を接続した場合

	$ ls /dev/pxmlt8video*
	/dev/pxmlt8video0  /dev/pxmlt8video3  /dev/pxmlt8video6
	/dev/pxmlt8video1  /dev/pxmlt8video4  /dev/pxmlt8video7
	/dev/pxmlt8video2  /dev/pxmlt8video5

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-M1UR を接続した場合

	$ ls /dev/pxm1urvideo*
	/dev/pxm1urvideo0

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-S1UR を接続した場合

	$ ls /dev/pxs1urvideo*
	/dev/pxs1urvideo0

すべてのチューナーにおいて、ISDB-T のみ受信可能です。

##### e-Better DTV03A-1TU を接続した場合

	$ ls /dev/isdbt2071video*
	/dev/isdbt2071video0

すべてのチューナーにおいて、ISDB-T のみ受信可能です。

##### e-Better DTV02-1T1S-U/DTV02A-1T1S-U を接続した場合

	$ ls /dev/isdb2056video*
	/dev/isdb2056video0

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

> [!NOTE]  
> ロット番号 2309 以降の DTV02A-1T1S-U を接続した場合でも、デバイスファイル名は `/dev/isdb2056video*` となります。  
> `/dev/isdb2056nvideo*` ではないので注意してください。

##### e-Better DTV02A-4TS-P を接続した場合

	$ ls /dev/isdb6014video*
	/dev/isdb6014video0  /dev/isdb6014video2
	/dev/isdb6014video1  /dev/isdb6014video3

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

### 4. 内蔵 B-CAS カードリーダーの利用

次の機種の内蔵カードリーダーは、pcscd 用のドライバー (`px4-drv-pcsc`) をインストールすると、外付けのカードリーダーと同じように、Mirakurun や EDCB などの PC/SC に対応したアプリケーションから使えます。  
内蔵カードリーダーを使わない場合は、この手順は不要です。

- PLEX PX-W3U4 / PX-Q3U4 / PX-W3PE4 / PX-Q3PE4 / PX-W3PE5 / PX-Q3PE5
- PLEX PX-MLT5PE / PX-MLT8PE
- PLEX PX-M1UR / PX-S1UR
- e-Better DTV02A-5TS-P / DTV02A-4TS-P / DTV02A-1T1S-U / DTV03A-1TU

> [!NOTE]
> Docker で Mirakurun などを動かしている場合は、pcscd をホストとコンテナのどちらで動かすかによって、ドライバーをインストールする場所が変わります。  
> 先に [Docker で使う場合](#docker-で使う場合) を読んでください。

#### pcscd 用ドライバーのインストール

Releases から、CPU アーキテクチャに合った Debian パッケージ (amd64 か arm64) をダウンロードし、pcscd と一緒にインストールします。  
Ubuntu 20.04 以降と、同じ世代以降の Debian 系ディストリビューションで動作します。

	$ wget https://github.com/tsukumijima/px4_drv/releases/download/v0.6.1/px4-drv-pcsc_0.6.1_amd64.deb
	$ sudo apt install -y ./px4-drv-pcsc_0.6.1_amd64.deb pcscd
	$ sudo udevadm trigger --subsystem-match=px4card
	$ sudo udevadm settle
	$ sudo systemctl restart pcscd

`udevadm` の2行は、接続済みのチューナーのカードリーダーを、新しくインストールした pcscd から開けるようにするためのものです。

> [!NOTE]
> 更新したときも、インストールの後に pcscd を再起動すると読み込まれます。  
> カードを使っている最中に再起動すると復号が途切れるため、録画や視聴をしていないときに実行してください。

> [!TIP]
> ソースからビルドしてインストールすることもできます。C++17 に対応したコンパイラー、make 、pkg-config 、pcsc-lite の開発用ファイル (Debian / Ubuntu では `libpcsclite-dev`) が必要です。  
> ```
> $ make -C pcsc
> $ sudo make -C pcsc install
> ```

#### 認識の確認

B-CAS カードを挿した状態で、`pcsc-tools` の `pcsc_scan` を実行します。

	$ sudo apt install -y pcsc-tools
	$ pcsc_scan

次のように、内蔵カードリーダーの名前の下に `Card inserted` と、末尾に `B-CAS Card` の判定が表示されれば使えます。  
確かめたら、Ctrl+C で終了します。

```
 Reader 0: PLEX PX-W3U4 Smart Card Reader [ID: CD65353E4231AA77] (000001711009061) 00 00
  Event number: 0
  Card state: Card inserted, 
  ATR: 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
(中略)
Possibly identified card (using /usr/share/pcsc/smartcard_list.txt):
3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
	Japanese Chijou Digital B-CAS Card (pay TV)
```

リーダー名の `[ID: ...]` は、Windows 版のリーダー名の `(ID: ...)` と同じ値です。  
Windows と Linux の両方で、同じ機器を共通の ID で識別できます。

#### 認識されない場合

- **カードリーダーが表示されない:** `ls /dev/px4card/` で、カードリーダーのデバイスファイルがあるかを確かめます。  
  ない場合は、インストールした px4_drv が内蔵カードリーダーに対応したバージョンかを確かめます。  
  ある場合は、[インストール](#pcscd-用ドライバーのインストール) の `udevadm` の2行と pcscd の再起動を、もう一度実行します
- **`Card state` が `Card inserted` にならない:** B-CAS カードの向き (裏表) と、奥まで挿し込まれているかを確かめます
- **リーダー名に `[ID: ...]` が入っていない:** カーネルモジュールを読み込み直したときなどに起きます。pcscd を再起動すると解消します

それでも表示されない場合は、`journalctl -u pcscd` で pcscd のログを確かめます。  
デバイスファイルを開けなかったときは、`open(...) failed` と、その理由が出ます。

#### root 以外のサービスから使う場合

Ubuntu 24.04 以降と Debian 13 以降の pcscd は、接続してきたプログラムを polkit で確かめ、ログインしていないユーザーからの接続を拒否します。  
このため、Mirakurun や EDCB を root 以外のユーザーの systemd サービスとして動かすと、カードリーダーを使えません (外付けのカードリーダーでも同じで、pcscd のログに `is NOT authorized for action: access_pcsc` が出ます)。  
Ubuntu 22.04 以前と Debian 12 以前の pcscd はこの確認をしないため、次の設定は不要です。

この拒否が出る場合は、次の polkit のルールを `/etc/polkit-1/rules.d/50-pcscd-recorder.rules` に置き、サービスのユーザー (この例では `mirakurun`) に許可します。

```js
polkit.addRule(function(action, subject) {
    if ((action.id == "org.debian.pcsc-lite.access_pcsc" ||
         action.id == "org.debian.pcsc-lite.access_card") &&
        subject.user == "mirakurun") {
        return polkit.Result.YES;
    }
});
```

> [!NOTE]
> 録画アプリケーションはカードリーダーのデバイスファイルを直接開かず、pcscd に接続します。  
> チューナーの `video` グループとは違い、アプリケーションのユーザーを `pcscd` グループに加える必要はありません。

#### Docker で使う場合

カーネルモジュールはホストにインストールします。  
pcscd は、ホストとコンテナのどちらか一方だけで動かし、それに合わせて pcscd 用ドライバーをインストールする場所を選びます。

- **コンテナ内で pcscd を動かす (Mirakurun の既定の構成):** コンテナのイメージにドライバーを追加してビルドし直します
- **ホストの pcscd を使う:** イメージはそのままで済みますが、ホストとコンテナの pcsc-lite のバージョンが大きく違うと接続できないことがあります

> [!WARNING]
> ホストとコンテナの両方で pcscd を動かすと、カードリーダーへのアクセスが競合し、片方が使えなくなります。

##### コンテナ内で pcscd を動かす

Mirakurun の公式イメージには pcscd 用ドライバーが入っていないので、Debian パッケージを追加したイメージを作ります。  
arm64 の機器 (Raspberry Pi など) では、ファイル名の `amd64` を `arm64` に変えます。

```dockerfile
FROM chinachu/mirakurun:latest
ADD https://github.com/tsukumijima/px4_drv/releases/download/v0.6.1/px4-drv-pcsc_0.6.1_amd64.deb /tmp/
RUN dpkg -i /tmp/px4-drv-pcsc_0.6.1_amd64.deb && rm /tmp/px4-drv-pcsc_0.6.1_amd64.deb
```

`docker-compose.yml` は Mirakurun の公式のもの (`/dev:/dev` の volume 、`network_mode: host` 、`device_cgroup_rules`) のまま、`image` だけをこのイメージに変えます。  
そのうえで、ホストの pcscd を止めます。

	$ sudo systemctl disable --now pcscd.socket pcscd.service

> [!NOTE]
> すべての機器を取り外すと `/dev/px4card` ごと消えるため、`/dev/px4card` だけをコンテナへマウントすると、抜き差しした後にコンテナから見えなくなります。  
> 公式の構成のとおり、`/dev` 全体をマウントしてください。

##### ホストの pcscd を使う

ホストに pcscd 用ドライバーを [インストール](#pcscd-用ドライバーのインストール) し、コンテナ内の pcscd は動かさずに、ホストの pcscd のソケットディレクトリをコンテナへマウントします。  
Mirakurun では、環境変数 `DISABLE_PCSCD=1` を設定し、`/var/run/pcscd:/var/run/pcscd` をマウントします。

> [!WARNING]
> Mirakurun の `docker-compose.yml` の例のように、ソケットファイル (`pcscd.comm`) だけをマウントすると、ホストの pcscd が再起動したときにコンテナから接続できなくなります。  
> ディレクトリごとマウントしてください。  
> また、ホストとコンテナの pcsc-lite のバージョンが大きく違うと接続できないことがあります。その場合は、コンテナ内で pcscd を動かす構成を使ってください。

## アンインストール (Windows)

### 1. ドライバのアンインストール

デバイスマネージャーからチューナーデバイス（ PLEX PX-W3PE5 ISDB-T/S Receiver Device (WinUSB) のような名前）を選択します。  
その後、右クリックメニュー → [デバイスのアンインストール] から、[このデバイスのドライバー ソフトウェアを削除します] にチェックを入れて、ドライバをアンインストールしてください。

### 2. 自己署名証明書のアンインストール

ドライバのインストール時にインストールした自己署名証明書をアンインストールするには、cert-uninstall.jse を実行します。アンインストールするとドライバが信頼されなくなってしまうので、十分注意してください。

## アンインストール (Linux)

### 1. ドライバのアンインストール

#### Debian パッケージを使用してインストールした場合

	$ sudo apt purge px4-drv-dkms

#### DKMS を使用してインストールした場合

	$ sudo dkms remove px4_drv/0.6.1 --all
	$ sudo rm -rf /usr/src/px4_drv-0.6.1

#### DKMS を使用せずにインストールした場合

内蔵カードリーダーを pcscd で使っている場合は、pcscd がデバイスファイルを開いたままでモジュールを外せないため、先に pcscd を止めます。

	$ sudo systemctl stop pcscd.socket pcscd.service
	$ sudo modprobe -r px4_drv
	$ cd driver
	$ sudo make uninstall
	$ cd ../

### 2. ファームウェアのアンインストール

> [!NOTE]  
> **px4_drv を Debian パッケージや DKMS を使用してインストールした場合、ファームウェアは自動的にアンインストールされます。**  
> Debian パッケージや DKMS を使用してインストールを行った場合は、この手順はスキップしてください。

	$ sudo rm /lib/firmware/it930x-firmware.bin

## 受信方法

### Windows

チューナーの機種に応じた BonDriver とその設定ファイルを配置し、TVTest や EDCB などの BonDriver に対応したソフトウェアで使用することで、TS データを受信することが可能です。

BonDriver は専用のものが必要になるため、公式 (Jacky版) BonDriver や radi-sh 氏版 BonDriver_BDA と併用することはできません。

### Linux

recpt1 や [BonDriverProxy_Linux](https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux) 等の PT シリーズ用 chardev ドライバに対応したソフトウェアを使用することで、TS データを受信することが可能です。  
recpt1 は、PLEX 社より配布されているものを使用する必要はありません。

BonDriverProxy_Linux と、PLEX PX-MLT5PE や e-Better DTV02A-1T1S-U などのデバイスファイル1つで ISDB-T と ISDB-S のどちらも受信可能なチューナーを組み合わせて使用する場合は、BonDriver として BonDriverProxy_Linux に同梱されている BonDriver_LinuxPT の代わりに、[BonDriver_LinuxPTX](https://github.com/nns779/BonDriver_LinuxPTX) を使用してください。

## LNB電源の出力

### PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4

出力なしと 15V の出力のみに対応しています。デフォルトでは LNB 電源の出力を行いません。  
LNB 電源の出力を行うには、recpt1 を実行する際のパラメータに `--lnb 15` を追加してください。

Windows では、BonDriver_PX4-S.ini に記載の `LNBPower=0` を `LNBPower=1` に変更してください。

### PLEX PX-W3PE5/Q3PE5

出力なしと 15V の出力のみに対応しているものと思われます。

### PLEX PX-MLT5PE/MLT8PE

対応していないものとされていましたが、5ch の有志により、正しく LNB 電源を出力できることが確認されています ([参考](https://mevius.5ch.net/test/read.cgi/avi/1648542476/267-288))。

### PLEX PX-M1UR / e-Better DTV02-1T1S-U/DTV02A-1T1S-U

対応しておりません。

### e-Better DTV02A-4TS-P

対応していると思われます。

### e-Better DTV02A-5TS-P

ハードウェア上 PX-MLT5PE と同一機種であることから、LNB 電源の動作も PX-MLT5PE と共通です。

## 備考

### 内蔵カードリーダーやリモコンについて

WinUSB 版と Linux 版のどちらでも、各種対応デバイスに内蔵されているカードリーダーを利用できます (Linux 版は pcsc-lite 用の [IFD ハンドラー](pcsc/README.md) の導入が必要です)。リモコンの操作には対応していません。

### e-Better DTV02-1T1S-U について

e-Better DTV02-1T1S-U は、個体によりデバイスからの応答が無くなることのある不具合が各所にて多数報告されています。そのため、このドライバでは「実験的な対応」とさせていただいております。  
上記の不具合はこの非公式ドライバでも完全には解消できないと思われますので、その点は予めご了承ください。

### e-Better DTV02A-1T1S-U について

e-Better DTV02A-1T1S-U は、DTV02-1T1S-U に存在した上記の不具合がハードウェアレベルで修正されています。そのため、このドライバでは「正式な対応」とさせていただいております。

## 技術情報

### デバイスの構成

PX-W3PE4/Q3PE4/MLT5PE/MLT8PE, e-Better DTV02A-4TS-P/DTV02A-5TS-P は、電源の供給を PCIe スロットから受け、データのやり取りを USB を介して行います。  
PX-W3PE5/Q3PE5 は、PX-W3PE4/Q3PE4 相当の基板に PCIe→USB ブリッジチップを追加し、USB ケーブルを不要とした構造となっています。  
PX-Q3U4/Q3PE4 は、PX-W3U4/W3PE4 相当のデバイスが USB ハブを介して2つぶら下がる構造となっています。

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

PX-MLT8PE は、同一基板上に PX-MLT5PE 相当のデバイスと、3チャンネル分のチューナーを持つデバイスが実装されている構造となっています。

- PX-MLT5PE/MLT8PE5 / DTV02A-5TS-P

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x5)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x5)

- PX-MLT8PE3

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x3)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x3)

DTV02-1T1S-U/DTV02A-1T1S-U は、ISDB-T 側の TS シリアル出力を ISDB-S 側と共有しています。そのため、同時に受信できるチャンネル数は1チャンネルのみです。

- DTV02-1T1S-U/DTV02A-1T1S-U (ロット番号 2309 以前: Digibest ISDB2056)

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90532XBG
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U では、ISDB-T/S Demodulator IC が TC90532XBG (ISDB-T×1TS + ISDB-S×1TS) から TC90522XBG (ISDB-T×2TS + ISDB-S×2TS) に変更されています。（[詳細](https://web.archive.org/web/20130513083035/http://www.semicon.toshiba.co.jp/product/new_products/assp/1275558_37644.html)）  
この変更に伴い、内部名称が ISDB2056 から ISDB2056N に変更されています。

- DTV02A-1T1S-U (ロット番号 2309 以降: Digibest ISDB2056N)

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (TC90532XBG -> TC90522XBG)
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

DTV02A-4TS-P は、PX-MLT5PE から1チャンネル分のチューナーを削減した構造となっています。

- DTV02A-4TS-P (Digibest ISDB6014)

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Sony CXD2856ER (x4)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x4)

### TS Aggregation の設定

sync_byte をデバイス側で書き換え、ホスト側でその値を元にそれぞれのチューナーの TS データを振り分けるモードを使用しています。
