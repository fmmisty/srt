const {Document,Packer,Paragraph,TextRun,HeadingLevel,TableOfContents,Table,TableRow,TableCell,WidthType,ShadingType,AlignmentType,PageBreak,LevelFormat}=require('docx');
const fs=require('fs'); const FONT="Yu Gothic",AC="1f4e79";
const h=(t,l)=>new Paragraph({heading:l,spacing:{before:220,after:110},children:[new TextRun({text:t,font:FONT})]});
const p=(t,o={})=>new Paragraph({spacing:{after:90},alignment:o.align,children:[new TextRun({text:t,font:FONT,size:o.size||21,italics:o.it})]});
const b=(t)=>new Paragraph({numbering:{reference:"bul",level:0},spacing:{after:50},children:[new TextRun({text:t,font:FONT,size:21})]});
const gap=()=>new Paragraph({children:[new TextRun("")],spacing:{after:70}});
function c(t,{w,head=false,al=AlignmentType.LEFT}={}){return new TableCell({width:{size:w,type:WidthType.DXA},shading:head?{type:ShadingType.CLEAR,fill:AC}:undefined,margins:{top:36,bottom:36,left:70,right:70},children:[new Paragraph({alignment:al,children:[new TextRun({text:t,font:FONT,size:18,bold:head,color:head?"FFFFFF":"000000"})]})]});}
function tbl(cols,rows){const tot=cols.reduce((a,x)=>a+x,0);const tr=[new TableRow({tableHeader:true,children:rows[0].map((t,i)=>c(t,{w:cols[i],head:true}))})];for(let r=1;r<rows.length;r++)tr.push(new TableRow({children:rows[r].map((t,i)=>c(t,{w:cols[i]}))}));return new Table({columnWidths:cols,width:{size:tot,type:WidthType.DXA},rows:tr});}
const A4={size:{width:11906,height:16838}};

const doc=new Document({
 styles:{default:{document:{run:{font:FONT,size:21}}}},
 numbering:{config:[{reference:"bul",levels:[{level:0,format:LevelFormat.BULLET,text:"•",alignment:AlignmentType.LEFT,style:{run:{font:FONT}},paragraphProperties:{indent:{left:360,hanging:200}}}]}]},
 sections:[{properties:{page:A4},children:[
  new Paragraph({spacing:{before:1600,after:160},alignment:AlignmentType.CENTER,children:[new TextRun({text:"SRT 音声コーデック",font:FONT,size:48,bold:true,color:AC})]}),
  new Paragraph({alignment:AlignmentType.CENTER,spacing:{after:100},children:[new TextRun({text:"1U ラックマウント / Raspberry Pi 内蔵",font:FONT,size:28})]}),
  new Paragraph({alignment:AlignmentType.CENTER,spacing:{after:1000},children:[new TextRun({text:"製品仕様書（案）",font:FONT,size:30})]}),
  new Paragraph({alignment:AlignmentType.CENTER,spacing:{after:60},children:[new TextRun({text:"バランスアナログ＋AES/EBU ／ SRT誤り訂正伝送 ／ 携帯WEBでコーデック無停止切替",font:FONT,size:20})]}),
  new Paragraph({alignment:AlignmentType.CENTER,spacing:{before:1800},children:[new TextRun({text:"RFデザイン株式会社",font:FONT,size:24})]}),
  new Paragraph({alignment:AlignmentType.CENTER,children:[new TextRun({text:"Rev.0.1 (2026-09) ／ 本書は設計確定前の暫定仕様（案）",font:FONT,size:18,color:"808080"})]}),
  new Paragraph({children:[new PageBreak()]}),

  h("目次",HeadingLevel.HEADING_1),
  new TableOfContents("目次",{hyperlink:true,headingStyleRange:"1-1"}),
  new Paragraph({children:[new PageBreak()]}),

  h("1. 概要",HeadingLevel.HEADING_1),
  p("本製品は、バランスアナログおよびAES/EBUの音声を、SRT（Secure Reliable Transport）で一般IP回線（フレッツ光・VPN・専用線・インターネット等）を通じて双方向伝送する、1UラックマウントのIP音声コーデックである。USBオーディオ・フロントエンド（UAC2準拠）とRaspberry Pi 4を内蔵し、1台で送受信が完結する。コーデックの切り替え・遅延設定はスマートフォンのブラウザから行える。"),
  p("SRTの誤り訂正（ARQ再送）と受信バッファにより、ベストエフォート回線でもパケットロス・ジッタに強い伝送を実現する。バランスアナログ＋AES/EBU＋ドライバ不要USB音声を1台に統合したRaspberry Piベースの製品は市場に存在せず、本製品がその用途を埋める。"),
  gap(),
  h("1.1 特長",HeadingLevel.HEADING_2),
  b("バランスアナログ(XLR)＋AES/EBU の入出力を装備"),
  b("SRTによる誤り訂正・ジッタ吸収で途切れにくい伝送（latency可変）"),
  b("5コーデック（LPCM／aptX／aptX HD／AAC／Opus）を、スマホのWEBから無停止で切替。受信側は自動追従"),
  b("全二重（送受信同時）。2台を対向させて双方向伝送"),
  b("Raspberry Pi 4 内蔵・1U完結。LAN／Wi-Fi対応"),

  h("2. システム構成",HeadingLevel.HEADING_1),
  p("拠点Aと拠点Bに本製品を各1台設置し、SRTで結ぶ。各機は内蔵Piで送信(srt_send)と受信(srt_recv)を同時実行する。"),
  p("音声 IN(アナログ/AES) → ADC/AES-RX → USB(UAC2) → 内蔵Pi(srt_send) → SRT送出", {size:19}),
  p("SRT受信 → 内蔵Pi(srt_recv) → USB(UAC2) → DAC/AES-TX → 音声 OUT(アナログ/AES)", {size:19}),
  p("操作: スマートフォン → Wi-Fi/LAN → 内蔵Pi(codec_web) でコーデック選択・現在表示。", {size:19}),
  p("※詳細な内部構成図は別紙（hardware/srt_codec_block.svg）参照。", {size:18,it:true}),

  h("3. 外観・寸法",HeadingLevel.HEADING_1),
  tbl([3200,5500],[
    ["項目","仕様"],
    ["形式","1U ラックマウント IP音声コーデック"],
    ["外形寸法","W 482.6 × H 44.45 (1U) × D 約200mm（暫定）"],
    ["質量","（設計確定後に確定）"],
    ["前面表示","POWER、OLED（サンプルレート/レベル）、USB LINK、AES LOCK、ANALOG IN/OUT SIG・CLIP"],
  ]),

  h("4. 入出力（背面）",HeadingLevel.HEADING_1),
  tbl([3000,5700],[
    ["コネクタ","内容"],
    ["AC IN","AC100–240V 50/60Hz（内部電源）"],
    ["LAN","RJ-45 10/100/1000。SRT伝送＋WEB操作"],
    ["USB-C","保守用（設定・更新・コンソール）"],
    ["AES/EBU IN","XLR-F、110Ω、AES3-1992、2ch"],
    ["AES/EBU OUT","XLR-M、110Ω、AES3-1992、2ch"],
    ["ANALOG IN L/R","XLR-F、バランス、+4dBu nominal／+20dBu max"],
    ["ANALOG OUT L/R","XLR-M、バランス、+4dBu nominal／+20dBu max"],
    ["Wi-Fi RF窓","樹脂カバー（レドーム）内側に内蔵アンテナ。スマホからの操作用（外付けUSB無線子機）"],
    ["REMOTE/GPIO","無し（本製品では非搭載）"],
  ]),

  h("5. 音声仕様",HeadingLevel.HEADING_1),
  tbl([3000,5700],[
    ["項目","仕様"],
    ["アナログ入出力","2ch バランス XLR、+4dBu nominal、+20dBu max（±15V電源、実力~+22dBu）"],
    ["AES/EBU","1系統 2ch、XLR 110Ω、トランス絶縁、AES3-1992"],
    ["サンプリング周波数","48kHz／96kHz"],
    ["ビット深度","A/D・D/A・AESは24bit処理。SRT伝送は16bit（S16LE）"],
    ["レベル設計","0dBFS ≒ +22dBu（+4dBu = −18dBFS、ヘッドルーム約18dB）"],
    ["A/D・D/A","PCM1862（ADC）／PCM5242（DAC）／THAT1646（出力ドライバ）"],
  ]),

  h("6. コーデック",HeadingLevel.HEADING_1),
  tbl([2600,2600,3500],[
    ["コーデック","目安ビットレート(48k/stereo)","用途"],
    ["LPCM（無圧縮）","約1,536 kbps","無劣化・帯域に余裕がある回線"],
    ["aptX HD","約576 kbps","高音質・低遅延"],
    ["aptX","約384 kbps","低遅延"],
    ["AAC","128 kbps〜","帯域節約・実績"],
    ["Opus","128 kbps〜","低遅延・高効率・特許フリー"],
  ]),
  p("コーデックはスマホのWEBから無停止で切替可能（受信側はSRTのCONFIGで自動追従）。切替時も音声は途切れない。", {size:19}),

  h("7. 伝送（SRT）",HeadingLevel.HEADING_1),
  tbl([3000,5700],[
    ["項目","仕様"],
    ["方式","SRT（Secure Reliable Transport）"],
    ["誤り訂正","ARQ（再送）によるパケットロス復元"],
    ["ジッタ耐性","受信バッファ（latency）で吸収"],
    ["latency","可変（目安：安定回線40〜80ms／フレッツ250ms／ベストエフォート300〜400ms）"],
    ["暗号化","対応可（パスフレーズ。自社閉域運用では省略可）"],
    ["接続","一方が待受、他方が接続。全二重（双方向同時）"],
  ]),

  h("8. ネットワーク・操作",HeadingLevel.HEADING_1),
  b("LAN(RJ-45)：SRT伝送および設定WEB"),
  b("Wi-Fi：外付けUSB無線子機＋パネルRF窓（樹脂カバー）内蔵アンテナ経由。スマートフォンからコーデック切替・状態確認（codec_web）"),
  b("USB-C：保守用"),
  b("操作アプリ：内蔵WEB（ブラウザのみ、専用アプリ不要）"),

  h("9. 主要構成部品",HeadingLevel.HEADING_1),
  tbl([3000,5700],[
    ["ブロック","部品"],
    ["USB/UAC2コントローラ","XMOS XU316（USB Audio Class 2.0）"],
    ["ADC / DAC","TI PCM1862 / PCM5242"],
    ["出力ラインドライバ","THAT1646（入力レシーバ THAT1246）"],
    ["AES/EBU 送受＋ASRC","TI SRC4392（RX＋TX＋デュアルASRC）"],
    ["内蔵コンピュータ","Raspberry Pi 4（2GB。量産時はCM4を検討）"],
    ["Wi-Fi","外付けUSB無線子機（技適済）＋パネルRF窓（樹脂カバー）内蔵アンテナ"],
    ["クロック","24.576MHz／22.5792MHz（低ジッタ）"],
  ]),

  h("10. 電源・環境",HeadingLevel.HEADING_1),
  tbl([3000,5700],[
    ["項目","仕様"],
    ["電源","AC100–240V 50/60Hz、内部DC電源"],
    ["消費電力","約25〜35W（内蔵Pi・電源込み、暫定）"],
    ["内部レール","+5V（Pi）／±15V（アナログ）／+3.3V／+0.9V（XU316コア）"],
    ["使用温度範囲","0〜+40℃（結露なきこと）"],
    ["冷却","ヒートシンク＋静音ファン／通気（密閉可否・詳細は設計確定後）"],
  ]),

  h("11. 付属ソフトウェア",HeadingLevel.HEADING_1),
  b("srt_send / srt_recv：マルチコーデックSRT送受信（無停止コーデック切替対応）"),
  b("codec_web.py：スマホ用コーデック切替WEB"),
  b("（自社利用版のためライセンス機能なし）"),

  h("12. 備考（設計確定前の暫定・未確定事項）",HeadingLevel.HEADING_1),
  b("筐体の密閉可否・冷却詳細（ファン/通気・放熱設計）"),
  b("量産構成（Raspberry Pi 4 ボード or CM4化）"),
  b("確定BOM・詳細回路図・質量・EMC/安全規格対応"),
  gap(),
  p("― 以上（本書は案。回路・BOM確定後に正式版へ改訂）―",{align:AlignmentType.CENTER}),
 ]}]
});
Packer.toBuffer(doc).then(b=>{fs.writeFileSync(process.argv[2]||"spec.docx",b);console.log("written",b.length);});
