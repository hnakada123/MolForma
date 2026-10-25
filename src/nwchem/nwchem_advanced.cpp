#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include <QHash>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

namespace mf {
namespace {
AdvancedOption numeric(QString key, QString group, QString label, QString keyword, QString help,
                       QString defaults, double minimum, double maximum, bool integer = false) {
    return {key,
            group,
            label,
            keyword,
            help,
            defaults,
            integer ? AdvancedKind::Integer : AdvancedKind::Real,
            minimum,
            maximum,
            {}};
}
AdvancedOption choice(QString key, QString group, QString label, QString keyword, QString help,
                      QVector<NwchemChoice> values, QString defaults = "NWChem既定") {
    return {key, group, label, keyword, help, defaults, AdvancedKind::Choice, 0, 0, values};
}
AdvancedOption flag(QString key, QString group, QString label, QString keyword, QString help) {
    return {key, group, label, keyword, help, "オフ（指定なし）", AdvancedKind::Flag, 0, 0, {}};
}
AdvancedOption path(QString key, QString label, QString keyword, QString help) {
    return {key, "files", label, keyword, help, "指定なし", AdvancedKind::Path, 0, 0, {}};
}
QString number(double value) {
    // Integer directives (iterations, scratchdisk, freeze, ...) reject exponent notation.
    if (std::isfinite(value) && std::floor(value) == value && std::abs(value) <= 1000000)
        return QString::number(value, 'f', 0);
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}
bool correlated(const QString &method) {
    return method == "mp2" || method.startsWith("ccsd");
}
bool realNumber(const QString &text, double &number) {
    QString normalized = text;
    normalized.replace('D', 'e').replace('d', 'e');
    if (!QRegularExpression("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")
             .match(normalized)
             .hasMatch())
        return false;
    bool ok;
    number = QLocale::c().toDouble(normalized, &ok);
    return ok && std::isfinite(number);
}
} // namespace
const QVector<AdvancedOption> &nwchemAdvancedOptions() {
    static const QVector<AdvancedOption> options{
        numeric("scf_thresh", "scf", "波動関数の収束閾値", "thresh",
                "「3 計算条件」のSCF収束を上書きします。", "3 計算条件に従う", 1e-12, 1e-2),
        numeric("scf_tol2e", "scf", "積分の打切り閾値", "tol2e",
                "二電子積分のスクリーニング精度を指定します。", "NWChemがthreshから決定", 1e-16, 1e-4),
        flag("scf_direct", "scf", "積分を毎回再計算", "direct",
             "SCFの積分キャッシュを使用しません。MP2/CCSD自身のディスク使用は別設定です。"),
        choice("scf_solver", "scf", "SCF収束法", "diis",
               "DIISは閉殻RHF専用です。開殻にはNWChem既定の収束法を使います。",
               {{"diis", "DIIS（閉殻RHF）"}}),
        numeric("scf_diisbas", "scf", "DIIS部分空間の大きさ", "diisbas",
                "上のSCF収束法でDIISを選んだ場合のみ使用します。", "5", 2, 50, true),
        numeric("dft_energy", "dft", "エネルギー収束閾値 / Hartree", "convergence energy",
                "「3 計算条件」のDFTエネルギー収束を上書きします。", "3 計算条件に従う", 1e-12, 1e-2),
        numeric("dft_density", "dft", "密度収束閾値", "convergence density",
                "DFT密度の収束閾値を個別に指定します。", "3 計算条件に従う", 1e-12, 1e-2),
        numeric("dft_gradient", "dft", "軌道勾配の収束閾値", "convergence gradient",
                "原子核の勾配ではなく、電子状態の軌道勾配です。", "3 計算条件に従う", 1e-12, 1e-2),
        numeric("dft_damp", "dft", "密度のダンピング / %", "convergence damp",
                "前反復の密度を混ぜる割合です。例: 70は前反復を70%使用します。", "NWChem既定", 0, 99, true),
        numeric("dft_ncydp", "dft", "初期ダンピングの反復数", "convergence ncydp",
                "初期段階でダンピングを行う反復数を指定します。", "2", 0, 10000, true),
        numeric("dft_lshift", "dft", "レベルシフト / Hartree", "convergence lshift",
                "空軌道ブロックの対角成分に加えるシフトです。", "0.5", 0, 10),
        numeric("dft_diis", "dft", "DIISのFock行列数", "convergence diis",
                "収束加速に保持するFock行列数です。増やすとメモリ使用量が増えます。", "10", 4, 50, true),
        flag("dft_direct", "dft", "DFT積分を毎回再計算", "direct", "DFTの積分キャッシュを使用しません。"),
        flag("dft_grid_nodisk", "dft", "グリッドをディスクに保存しない", "grid nodisk",
             "グリッド精度は「3 計算条件」のfine/xfineを使用します。"),
        numeric("corr_freeze", "correlation", "凍結する占有軌道数", "freeze",
                "「3 計算条件」のfreeze atomicを上書きします。0は凍結なし。最低エネルギー側から数えます。",
                "3 計算条件に従う", 0, 100000, true),
        flag("mp2_tight", "correlation", "MP2の精度を厳密にする", "tight",
             "MP2の積分・SCF・CPHF精度を厳密にします。"),
        numeric("mp2_scratchdisk", "correlation", "MP2作業ディスク / MB・プロセス", "scratchdisk",
                "半直接MP2の積分変換用ディスク容量を制限します。", "NWChem既定", 1, 1000000, true),
        numeric("cc_thresh", "correlation", "CC反復の収束閾値", "thresh",
                "CCSD/CCSD(T)のエネルギーと振幅の収束を制御します。", "1e-7（MolForma）", 1e-12, 1e-3),
        numeric("cc_tol2e", "correlation", "CC積分の打切り閾値", "tol2e",
                "NWChemのCCSDで調整可能な積分に適用します。", "NWChem既定", 1e-16, 1e-12),
        numeric("cc_diisbas", "correlation", "CC DIIS部分空間", "diisbas",
                "大きくするとCC反復のメモリ使用量が増えます。", "5", 2, 50, true),
        flag("cc_nodisk", "correlation", "CC積分をディスクに保存しない", "nodisk",
             "必要な積分を再計算します。SCFのdirect設定とは独立です。"),
        numeric("driver_gmax", "driver", "勾配の最大成分 / Hartree・Bohr⁻¹", "gmax",
                "最適化の収束閾値を個別に上書きします。", "3 計算条件の最適化収束に従う", 1e-9, 0.1),
        numeric("driver_grms", "driver", "勾配のRMS / Hartree・Bohr⁻¹", "grms",
                "最適化の勾配RMSの収束条件です。", "3 計算条件に従う", 1e-9, 0.1),
        numeric("driver_xmax", "driver", "変位の最大成分 / Bohr", "xmax",
                "Cartesian変位の最大成分に対する収束条件です。", "3 計算条件に従う", 1e-9, 1),
        numeric("driver_xrms", "driver", "変位のRMS / Bohr", "xrms",
                "Cartesian変位のRMSに対する収束条件です。", "3 計算条件に従う", 1e-9, 1),
        numeric("driver_trust", "driver", "最適化のステップ幅 / Bohr", "trust",
                "DRIVERのtrust radius。座標の出力単位によらず原子単位です。", "0.3", 1e-4, 2),
        numeric("driver_eprec", "driver", "エネルギーの計算精度 / Hartree", "eprec",
                "線形探索で想定するエネルギー精度です。収束条件そのものではありません。",
                "HF等: 1e-7 / DFT: 5e-6", 1e-12, 1e-3),
        flag("driver_xyz", "driver", "最適化の各構造をXYZに出力", "xyz",
             "「3 計算条件」の計算名をファイル名の接頭辞に使用します。"),
        flag("driver_cartesian", "driver", "Cartesian座標で最適化（noautoz）", "noautoz",
             "固定原子・ゴーストのない最適化は、NWChemの自動内部座標（autoz）で行います。弱く結合した"
             "錯体などで収束しにくい場合に、Cartesian座標へ切り替えます。"),
        {"freq_temperatures",
         "frequency",
         "熱化学の温度 / K",
         "temp",
         "空白区切りで1〜20個指定します。温度数は自動出力します。",
         "298.15",
         AdvancedKind::Temperatures,
         1,
         10000,
         {}},
        numeric("freq_fd_delta", "frequency", "数値Hessianの差分幅 / Bohr", "fd_delta",
                "数値Hessianを使用する場合のみ有効です。解析Hessianでは保留します。",
                "DFT: 0.010 / その他: 0.001", 1e-5, 0.1),
        numeric("hessian_thresh", "frequency", "解析Hessianの波動関数精度", "hessian thresh",
                "解析Hessianに必要なSCF/DFTの精度を指定します。", "1e-6", 1e-12, 1e-3),
        flag("freq_animate", "frequency", "振動モードのXYZを出力", "animate",
             "各振動モードのアニメーション用XYZを生成します。計算名を接頭辞に使用します。"),
        numeric("cosmo_minbem", "cosmo", "空洞表面の分割レベル", "minbem",
                "大きくすると表面分割が細かくなります。このUIでは0〜4を扱います。", "2", 0, 4, true),
        choice("cosmo_ificos", "cosmo", "空洞表面の初期多面体", "ificos",
               "正二十面体は正八面体より細かい表面になります。", {{"0", "正八面体"}, {"1", "正二十面体"}}),
        choice("cosmo_screen", "cosmo", "誘電遮蔽のスケーリング", "screen",
               "idealでは「3 計算条件」の誘電率を使用しません。",
               {{"st", "Stefanovich–Truong"}, {"ks", "Klamt–Schüürmann"}, {"ideal", "完全導体（ideal）"}}),
        numeric("cosmo_zeta", "cosmo", "表面電荷の幅", "zeta",
                "York–Karplus COSMOのGaussian表面電荷幅を制御します。", "0.98", 0.01, 10),
        choice("vectors_guess", "files", "初期分子軌道", "vectors input",
               "相関計算では参照SCFに適用します。ファイル指定では原子順・基底との整合を実行時に確認してくださ"
               "い。",
               {{"atomic", "原子密度の重ね合わせ"},
                {"hcore", "一電子Hamiltonian"},
                {"file", "既存movecsファイル"}}),
        path("vectors_input", "入力movecsファイル", "vectors input",
             "初期分子軌道がファイル指定のときに使用します。パスは計算実行先を基準とし、ここではファイルを読"
             "みません。"),
        path("vectors_output", "出力movecsファイル", "vectors output",
             "相対パスはpermanent内、./で始めるとジョブ内に保存します。既存movecsを読む場合は入力とは別の"
             "出力先を指定してください。それ以外は空欄でNWChem既定です。"),
        path("scratch_dir", "作業ファイルのディレクトリ", "scratch_dir",
             "実行先に存在する、空白・:を含まないパスを指定します。MolFormaは作成しません。"),
        path("permanent_dir", "永続ファイルのディレクトリ", "permanent_dir",
             "データベース・軌道等の保存先。空白・:は未対応です。環境変数は展開しません。"),
        flag("echo", "files", "入力を出力ログへ表示", "echo", "NWChemの入力エコーを有効にします。"),
        flag("molden_output", "visualization", "MOLDENファイルを出力", "property moldenfile",
             "計算終了後のHF/KS分子軌道を〈計算名〉.moldenへ出力します。MP2/CCSDでは参照HFの軌道です。"),
        choice("molden_norm", "visualization", "MOLDENの基底正規化", "molden_norm",
               "NWChem規約または未正規化。利用先の解析ソフトの規約に合わせて指定します。",
               {{"nwchem", "NWChem規約"}, {"none", "未正規化（NONE）"}}, "NWChem規約"),
        flag("cube_output", "visualization", "CUBEファイルを出力", "dplot gaussian",
             "計算終了後のHF/KS電子密度または分子軌道を出力します。MP2/CCSDの相関密度は出力しません。"),
        choice("cube_type", "visualization", "CUBEの内容", "spin / orbitals view",
               "密度は占有数を反映し、分子軌道は符号付きの軌道関数を出力します。",
               {{"density", "全電子密度（Alpha + Beta）"},
                {"alpha_density", "Alpha電子密度"},
                {"beta_density", "Beta電子密度"},
                {"spin_density", "スピン密度（Alpha − Beta）"},
                {"orbital", "分子軌道"}},
               "全電子密度"),
        choice("cube_spin", "visualization", "分子軌道のスピン", "spin",
               "Betaの軌道関数はUHF/UKSで指定できます。制限計算ではAlphaを使ってください。",
               {{"alpha", "Alpha"}, {"beta", "Beta（非制限計算）"}}, "Alpha"),
        choice("cube_orbital", "visualization", "出力する分子軌道", "orbitals view",
               "HOMO/LUMOは電子数・多重度とスピンから番号を決めます。番号指定は1から数えます。",
               {{"homo", "HOMO"}, {"lumo", "LUMO"}, {"index", "軌道番号で指定"}}, "HOMO"),
        numeric("cube_index", "visualization", "分子軌道番号", "orbitals view; 1; index",
                "番号指定時に必要です。軌道の総数は実行時の基底に依存し、NWChemが確認します。",
                "番号を指定してください", 1, 100000, true),
        {"cube_filename",
         "visualization",
         "CUBEファイル名",
         "output",
         "英数字・_・-・.で指定し、拡張子は.cubeまたは.cub。ディレクトリは永続ファイルの設定に従います。",
         "〈計算名〉_〈内容・軌道番号〉.cube",
         AdvancedKind::Path,
         0,
         0,
         {}},
        numeric(
            "cube_padding", "visualization", "入力構造からの余白 / Å", "limitxyz units angstrom",
            "空欄の軸範囲は入力構造の最小・最大座標にこの余白を加えます。最適化後の範囲を自動追跡しません。",
            "3", 0.1, 100),
        numeric("cube_divisions", "visualization", "最長辺の分割数", "limitxyz",
                "格子間隔は全軸で同じで、最も長い辺をこの数で分割します（各軸の格子点数は分割数+1）。"
                "X/Y/Zの個別指定を優先します。全体で200万格子点まで。",
                "40", 1, 1000, true),
        {"cube_x",
         "visualization",
         "X範囲・分割数",
         "limitxyz X",
         "最小値 最大値 分割数を空白区切りで指定。座標はÅ。例: -3 3 40。",
         "入力構造＋余白・共通の格子間隔",
         AdvancedKind::GridAxis,
         -100000,
         100000,
         {}},
        {"cube_y",
         "visualization",
         "Y範囲・分割数",
         "limitxyz Y",
         "最小値 最大値 分割数を空白区切りで指定。座標はÅ。例: -3 3 40。",
         "入力構造＋余白・共通の格子間隔",
         AdvancedKind::GridAxis,
         -100000,
         100000,
         {}},
        {"cube_z",
         "visualization",
         "Z範囲・分割数",
         "limitxyz Z",
         "最小値 最大値 分割数を空白区切りで指定。座標はÅ。例: -3 3 40。",
         "入力構造＋余白・共通の格子間隔",
         AdvancedKind::GridAxis,
         -100000,
         100000,
         {}}};
    return options;
}
QVector<NwchemChoice> nwchemAdvancedGroups() {
    return {{"scf", "HF / 参照SCF"},         {"dft", "DFT"},
            {"correlation", "MP2 / CCSD"},   {"driver", "DRIVER 最適化"},
            {"frequency", "振動数・熱化学"}, {"cosmo", "COSMO"},
            {"files", "軌道・ファイル"},     {"visualization", "可視化ファイル出力（CUBE / MOLDEN）"}};
}
QJsonObject nwchemAdvanced(const Document &d) {
    return d.data["nwchem"].toObject()["advanced"].toObject();
}
bool nwchemAdvancedApplies(const AdvancedOption &o, const Document &d) {
    const auto s = nwchemSettings(d);
    const auto a = nwchemAdvanced(d);
    const auto method = s["method"].toString(), task = s["task"].toString();
    if (o.group == "visualization") {
        if (o.key == "cube_output" || o.key == "molden_output")
            return true;
        if (o.key == "molden_norm")
            return a["molden_output"].toBool();
        if (!a["cube_output"].toBool())
            return false;
        if (o.key == "cube_index")
            return a["cube_type"] == "orbital" && a["cube_orbital"] == "index";
        if (o.key == "cube_spin" || o.key == "cube_orbital")
            return a["cube_type"] == "orbital";
        if (o.key == "cube_padding" || o.key == "cube_divisions")
            return !a.contains("cube_x") || !a.contains("cube_y") || !a.contains("cube_z");
        return true;
    }
    if (o.key == "scf_diisbas")
        return method != "dft" && a["scf_solver"] == "diis";
    if (o.key == "vectors_input")
        return a["vectors_guess"] == "file";
    if (o.key == "freq_fd_delta")
        return task.contains("frequencies") && nwchemNumericalFrequencies(d);
    if (o.key == "hessian_thresh")
        return task.contains("frequencies") && !nwchemNumericalFrequencies(d);
    if (o.key == "driver_cartesian") // Constraints and relaxed scans need internal coordinates.
        return nwchemDriverTask(s) && task != "scan" && s["constraints"].toArray().isEmpty();
    if (o.group == "scf")
        return method != "dft";
    if (o.group == "dft")
        return method == "dft";
    if (o.group == "correlation")
        return o.key.startsWith("mp2_")  ? method == "mp2"
               : o.key.startsWith("cc_") ? method.startsWith("ccsd")
                                         : correlated(method);
    if (o.group == "driver")
        return nwchemDriverTask(s);
    if (o.group == "frequency")
        return task.contains("frequencies");
    if (o.group == "cosmo")
        return s["solvent"] != "none" && (method == "hf" || method == "dft");
    return true;
}
QString nwchemAdvancedScope(const AdvancedOption &o) {
    // In the order of nwchemAdvancedApplies.
    if (o.group == "visualization") {
        if (o.key == "cube_output" || o.key == "molden_output")
            return "すべての計算";
        if (o.key == "molden_norm")
            return "MOLDEN出力がオンのとき";
        if (o.key == "cube_index")
            return "CUBEで分子軌道を番号指定するとき";
        if (o.key == "cube_spin" || o.key == "cube_orbital")
            return "CUBEの内容が分子軌道のとき";
        if (o.key == "cube_padding" || o.key == "cube_divisions")
            return "CUBE出力（X/Y/Zの範囲に空欄があるとき）";
        return "CUBE出力がオンのとき";
    }
    if (o.key == "scf_diisbas")
        return "HF・MP2・CCSD（SCF収束法がDIISのとき）";
    if (o.key == "vectors_input")
        return "初期分子軌道がファイル指定のとき";
    if (o.key == "freq_fd_delta")
        return "数値Hessianを使う振動数";
    if (o.key == "hessian_thresh")
        return "解析Hessianを使う振動数";
    if (o.key == "driver_cartesian")
        return "構造最適化・遷移状態探索（内部座標の拘束・スキャンなし）";
    if (o.group == "scf")
        return "HF・MP2・CCSD（参照SCF）";
    if (o.group == "dft")
        return "DFTの計算";
    if (o.group == "correlation")
        return o.key.startsWith("mp2_")  ? "MP2"
               : o.key.startsWith("cc_") ? "CCSD・CCSD(T)"
                                         : "MP2・CCSD・CCSD(T)";
    if (o.group == "driver")
        return "構造最適化・遷移状態探索・緩和スキャン";
    if (o.group == "frequency")
        return "振動数を含む計算";
    if (o.group == "cosmo")
        return "溶媒（COSMO・SMD）を使うHF・DFT";
    return "すべての計算";
}
QString nwchemAdvancedDefault(const AdvancedOption &o, const Document &d) {
    const auto s = nwchemSettings(d);
    const auto label = [](const QString &key, const QString &value) {
        for (const auto &c : nwchemChoices(key))
            if (c.value == value)
                return c.label;
        return value;
    };
    if (QStringList{"scf_thresh", "dft_energy", "dft_density", "dft_gradient"}.contains(o.key)) {
        const auto threshold = nwchemScfThreshold(s);
        return (threshold.isEmpty() ? QString("NWChem既定") : threshold) +
               "（3 計算条件のSCF収束: " + label("scf_convergence", s["scf_convergence"].toString()) + "）";
    }
    if (o.key == "corr_freeze")
        return s["freeze_core"].toBool() ? "atomic（3 計算条件の内殻凍結）" : "0（凍結なし）";
    // NWChem's DRIVER criteria for the keyword MolForma writes (default, tight, loose).
    static const QHash<QString, std::array<const char *, 4>> criteria{
        {"default", {"0.00045", "0.0003", "0.0018", "0.0012"}},
        {"tight", {"0.000015", "0.00001", "0.00006", "0.00004"}},
        {"loose", {"0.0045", "0.003", "0.018", "0.012"}}};
    static const QStringList driver{"driver_gmax", "driver_grms", "driver_xmax", "driver_xrms"};
    if (const int index = driver.indexOf(o.key); index >= 0) {
        const auto convergence = s["opt_convergence"].toString();
        if (criteria.contains(convergence))
            return QString(criteria[convergence][index]) +
                   "（3 計算条件の最適化収束: " + label("opt_convergence", convergence) + "）";
    }
    const bool dft = s["method"] == "dft";
    if (o.key == "driver_eprec")
        return dft ? "5e-6（DFT）" : "1e-7";
    if (o.key == "freq_fd_delta")
        return dft ? "0.010（DFT）" : "0.001";
    // A bare number is NWChem's own default; the output options' numbers are MolForma's.
    bool number = false;
    o.defaultText.toDouble(&number);
    return number && o.group != "visualization" ? o.defaultText + "（NWChem既定）" : o.defaultText;
}
QString nwchemAdvancedBlock(const AdvancedOption &o, const Document &d) {
    const auto method = nwchemSettings(d)["method"].toString();
    if (o.key.startsWith("vectors_"))
        return method == "dft" ? "dft" : "scf";
    if (o.key == "driver_cartesian")
        return "geometry";
    if (o.key == "hessian_thresh")
        return "hessian";
    if (o.group == "files")
        return o.keyword;
    if (o.group == "correlation")
        return method == "mp2" ? "mp2" : "ccsd";
    if (o.group == "frequency")
        return "freq";
    if (o.group == "visualization")
        return o.key.startsWith("molden_") ? "property" : "dplot";
    return o.group;
}
QString nwchemAdvancedError(const AdvancedOption &o, const QJsonValue &v) {
    if (v.isUndefined())
        return {};
    if (o.kind == AdvancedKind::Real || o.kind == AdvancedKind::Integer) {
        double n = v.toDouble();
        if (!v.isDouble() || !std::isfinite(n) || n < o.minimum || n > o.maximum ||
            (o.kind == AdvancedKind::Integer && std::floor(n) != n))
            return QString("%1〜%2の%3を指定してください。")
                .arg(number(o.minimum), number(o.maximum), o.kind == AdvancedKind::Integer ? "整数" : "数値");
    } else if (o.kind == AdvancedKind::Flag) {
        if (!v.isBool())
            return "真偽値を指定してください。";
    } else if (o.kind == AdvancedKind::Choice) {
        if (!v.isString() || std::none_of(o.choices.begin(), o.choices.end(),
                                          [&](const auto &c) { return c.value == v.toString(); }))
            return "対応していない選択値です。";
    } else if (o.kind == AdvancedKind::Path) {
        const auto text = v.toString();
        if (!v.isString() || text.trimmed().isEmpty() || text.size() > 240 ||
            text.contains(QRegularExpression("[\\x00-\\x1f\\x7f\"\\\\;#]")))
            return "パスは240文字以内で、改行・引用符・\\・;・#を含めず指定してください。";
        // Startup directory directives use a different parser from VECTORS:
        // quotes are literal, whitespace separates paths and ':' introduces a host/process selector.
        if (o.key.endsWith("_dir") && text.contains(QRegularExpression("[\\s:']")))
            return "ディレクトリには空白・:・引用符を含まない単一のパスを指定してください。";
        if (o.key == "vectors_input" &&
            QStringList{"atomic", "hcore", "project", "fragment"}.contains(text.toLower()))
            return "予約語と同じファイル名には ./ を付けてください。";
        if (o.key == "cube_filename" && !QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,114}\\.(cube|cub)$",
                                                            QRegularExpression::CaseInsensitiveOption)
                                             .match(text)
                                             .hasMatch())
            return "パスを含めず、英数字で始まる120文字以内の.cube / .cubファイル名を指定してください。";
    } else if (o.kind == AdvancedKind::GridAxis) {
        const auto values = v.toArray();
        if (!v.isArray() || values.size() != 3)
            return "最小値 最大値 分割数の3個を指定してください。";
        for (const auto &value : values)
            if (!value.isDouble() || !std::isfinite(value.toDouble()))
                return "有限の数値を指定してください。";
        if (values[0].toDouble() < o.minimum || values[1].toDouble() > o.maximum ||
            values[0].toDouble() >= values[1].toDouble())
            return "範囲は−100000〜100000 Åで、最小値 < 最大値にしてください。";
        const auto n = values[2].toDouble();
        if (n < 1 || n > 1000 || n != std::floor(n))
            return "分割数は1〜1000の整数にしてください。";
    } else if (o.kind == AdvancedKind::Temperatures) {
        if (!v.isArray() || v.toArray().isEmpty() || v.toArray().size() > 20)
            return "温度を空白区切りで1〜20個指定してください。";
        for (const auto &item : v.toArray())
            if (!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble() < o.minimum ||
                item.toDouble() > o.maximum)
                return "温度は1〜10000 Kで指定してください。";
    }
    return {};
}
QJsonValue parseNwchemAdvanced(const AdvancedOption &o, const QString &text) {
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QJsonValue::Undefined;
    if (o.kind == AdvancedKind::Real || o.kind == AdvancedKind::Integer) {
        double n;
        return realNumber(trimmed, n) ? QJsonValue(n) : QJsonValue(trimmed);
    }
    if (o.kind == AdvancedKind::Temperatures || o.kind == AdvancedKind::GridAxis) {
        QJsonArray values;
        for (const auto &part : trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
            double n;
            if (!realNumber(part, n))
                return trimmed;
            values.append(n);
        }
        return values;
    }
    return trimmed;
}
QString nwchemAdvancedText(const QJsonValue &v) {
    if (v.isDouble())
        return number(v.toDouble());
    if (v.isArray()) {
        QStringList parts;
        for (const auto &n : v.toArray())
            parts << nwchemAdvancedText(n);
        return parts.join(' ');
    }
    if (v.isObject())
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isNull())
        return "null";
    return v.toVariant().toString();
}
void setNwchemAdvanced(Document &d, const QString &key, const QJsonValue &value) {
    auto s = d.data.value("nwchem").toObject(), a = s.value("advanced").toObject();
    if (value.isUndefined())
        a.remove(key);
    else
        a[key] = value;
    if (a.isEmpty())
        s.remove("advanced");
    else
        s["advanced"] = a;
    if (!s.isEmpty() || d.data.contains("nwchem"))
        d.data["nwchem"] = s;
}
void resetNwchemAdvanced(Document &d, const QString &group) {
    for (const auto &o : nwchemAdvancedOptions())
        if (group.isEmpty() || o.group == group)
            setNwchemAdvanced(d, o.key, QJsonValue::Undefined);
}
void validateNwchemAdvanced(const Document &d, NwchemValidation &r) {
    const auto s = nwchemSettings(d), a = nwchemAdvanced(d);
    if (s.contains("advanced") && !s["advanced"].isObject())
        r.errors << "NWChem詳細設定はオブジェクトである必要があります。";
    QStringList pending;
    QSet<QString> known;
    for (const auto &o : nwchemAdvancedOptions()) {
        known.insert(o.key);
        if (!a.contains(o.key))
            continue;
        if (!nwchemAdvancedApplies(o, d)) {
            pending << o.label;
            continue;
        }
        const auto error = nwchemAdvancedError(o, a[o.key]);
        if (!error.isEmpty())
            r.errors << "詳細設定「" + o.label + "」: " + error;
    }
    for (auto it = a.begin(); it != a.end(); ++it)
        if (!known.contains(it.key()))
            r.warnings << "未対応の詳細設定を保持していますが出力しません: " + it.key();
    if (!pending.isEmpty())
        r.warnings << "現在の計算では保留（入力に出力しません）: " + pending.join("、");
    if (s["method"] != "dft" && a["scf_solver"] == "diis" && r.reference != "rhf")
        r.errors << "NWChemのSCF DIISは閉殻RHFでのみ使用できます。SCF収束法を既定へ戻してください。";
    if (a["vectors_guess"] == "file") {
        if (!a.contains("vectors_input"))
            r.errors << "初期分子軌道のmovecsファイルを指定してください。";
        else
            r.warnings << "初期軌道ファイルは計算実行先に必要です。原子順・基底との整合を確認してください。";
        if (!a.contains("vectors_output"))
            r.errors << "初期軌道を上書きしないよう、別の出力movecsファイルを指定してください。";
    }
    if (a.contains("vectors_input") && a["vectors_guess"] == "file" &&
        a["vectors_input"] == a["vectors_output"])
        r.errors << "入力・出力movecsには別のファイル名を指定してください。";
    if (s["solvent"] != "none" && a["cosmo_screen"] == "ideal")
        r.warnings << "COSMOのideal指定により、比誘電率の設定は使用しません。";
    validateNwchemOutput(d, r);
}
QString nwchemAdvancedNumber(const Document &d, const QString &key, const QString &fallback) {
    const auto a = nwchemAdvanced(d);
    return a.contains(key) ? number(a[key].toDouble()) : fallback;
}
QString nwchemAdvancedLines(const Document &d, const QString &group) {
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    QString text;
    for (const auto &o : nwchemAdvancedOptions()) {
        if (o.group == "visualization" || o.group != group || !a.contains(o.key) ||
            !nwchemAdvancedApplies(o, d))
            continue;
        if (!nwchemAdvancedError(o, a[o.key]).isEmpty())
            fail("不正なNWChem詳細設定: " + o.key);
        // These replace existing directives, or need structured output below.
        if (QStringList{"scf_thresh", "dft_energy", "dft_density", "dft_gradient", "dft_grid_nodisk",
                        "corr_freeze", "cc_thresh", "vectors_guess", "vectors_input", "vectors_output",
                        "freq_animate", "hessian_thresh", "driver_xyz", "driver_cartesian"}
                .contains(o.key))
            continue;
        if (o.kind == AdvancedKind::Flag) {
            if (a[o.key].toBool())
                text += "  " + o.keyword + '\n';
        } else if (o.kind == AdvancedKind::Temperatures)
            text += "  temp " + QString::number(a[o.key].toArray().size()) + ' ' +
                    nwchemAdvancedText(a[o.key]) + '\n';
        else if (o.kind == AdvancedKind::Path)
            text += "  " + o.keyword + ' ' + a[o.key].toString() + '\n'; // Startup directories only.
        else if (o.key == "scf_solver")
            text += "  diis\n";
        else
            text += "  " + o.keyword + ' ' + nwchemAdvancedText(a[o.key]) + '\n';
    }
    if (group == "vectors") {
        const auto guess = a["vectors_guess"].toString();
        QString line;
        if (!guess.isEmpty())
            line += " input " + (guess == "file" ? "\"" + a["vectors_input"].toString() + "\"" : guess);
        if (a.contains("vectors_output"))
            line += " output \"" + a["vectors_output"].toString() + "\"";
        if (!line.isEmpty())
            text += "  vectors" + line + '\n';
    }
    if (group == "frequency" && a["freq_animate"].toBool())
        text += "  animate\n  filename " + s["start"].toString() + "_vib\n";
    if (group == "driver" && a["driver_xyz"].toBool())
        text += "  xyz " + s["start"].toString() + "_opt\n";
    return text;
}
QJsonObject nwchemAdvancedPreset(const QJsonObject &settings) {
    QJsonObject result;
    for (const auto &o : nwchemAdvancedOptions())
        if (o.group != "files" && o.group != "visualization" && settings.contains(o.key))
            result[o.key] = settings[o.key];
    return result;
}
void applyNwchemAdvancedPreset(Document &d, const QJsonObject &settings) {
    for (const auto &o : nwchemAdvancedOptions())
        if (o.group != "files" && o.group != "visualization")
            setNwchemAdvanced(d, o.key, settings.value(o.key));
}
} // namespace mf
