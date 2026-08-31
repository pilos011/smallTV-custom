"""ICAO airline designator -> Korean name, and the glyphs that costs.

The callsign the feed gives is an ICAO flight identifier: three letters of
airline designator followed by the flight number. Turning the prefix into a name
needs nothing but a lookup, so the table is compiled in.

Kept to carriers that actually show up over Korea plus the long-haul names a
reader would recognise. Anything not listed keeps showing the raw callsign,
which is the honest fallback: a wrong airline name is worse than none.
"""
import io
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")

FW = "D:/Personal/SmallTV-Custom/firmware/sdpro-clock-weather/"

# ICAO three-letter designator -> the name to show.
AIRLINES = [
    # Korean carriers
    ("KAL", "대한항공"),
    ("AAR", "아시아나항공"),
    ("JJA", "제주항공"),
    ("JNA", "진에어"),
    ("TWB", "티웨이항공"),
    ("ABL", "에어부산"),
    ("ESR", "이스타항공"),
    ("ASV", "에어서울"),
    ("APZ", "에어프레미아"),
    ("EOK", "에어로케이"),
    ("XUM", "섬에어"),
    # Japan
    ("JAL", "일본항공"),
    ("ANA", "전일본공수"),
    ("APJ", "피치항공"),
    ("SKY", "스카이마크"),
    ("JJP", "제트스타재팬"),
    ("SNJ", "솔라시드에어"),
    ("JTA", "일본트랜스오션"),
    ("SFJ", "스타플라이어"),
    ("ADO", "에어두"),
    # Greater China
    ("CCA", "중국국제항공"),
    ("CES", "중국동방항공"),
    ("CSN", "중국남방항공"),
    ("CPA", "캐세이퍼시픽"),
    ("CAL", "중화항공"),
    ("EVA", "에바항공"),
    ("CHH", "하이난항공"),
    # Southeast Asia and beyond
    ("SIA", "싱가포르항공"),
    ("THA", "타이항공"),
    ("HVN", "베트남항공"),
    ("PAL", "필리핀항공"),
    ("MAS", "말레이시아항공"),
    ("GIA", "가루다인도네시아"),
    ("AIC", "에어인디아"),
    ("TVJ", "타이비엣젯"),
    # Middle East
    ("UAE", "에미레이트"),
    ("QTR", "카타르항공"),
    ("ETD", "에티하드"),
    ("SVA", "사우디아항공"),
    ("THY", "터키항공"),
    # Europe
    ("DLH", "루프트한자"),
    ("AFR", "에어프랑스"),
    ("KLM", "케이엘엠"),
    ("BAW", "브리티시항공"),
    ("SWR", "스위스항공"),
    ("AFL", "아에로플로트"),
    ("FIN", "핀에어"),
    # North America
    ("DAL", "델타항공"),
    ("UAL", "유나이티드항공"),
    ("AAL", "아메리칸항공"),
    ("ACA", "에어캐나다"),
    # Cargo, which is a lot of what flies at night
    ("FDX", "페덱스"),
    ("UPS", "유피에스"),
    ("GTI", "아틀라스항공"),
    ("CLX", "카고룩스"),
    ("ABW", "에어브리지카고"),
    ("CKS", "칼리타에어"),
    ("CSS", "순펑항공"),
    ("CAO", "에어차이나카고"),

    # Found by sampling 250 nm of live traffic and asking the route service who
    # each unnamed prefix belongs to, rather than guessing from the letters.
    # Two that turned up are still missing on purpose: CARD** is a military
    # tactical callsign rather than an operator, and HLC is on record nowhere.
    # They keep showing their raw callsign, which cannot be wrong. (CDC was on
    # this list too until the airport's own sheet named it: Loong Air.)
    ("CDG", "산둥항공"),
    ("DKH", "준야오항공"),
    ("KZR", "에어아스타나"),
    ("SBI", "S7항공"),
    ("SJO", "스프링재팬"),
    ("VJC", "비엣젯"),
    ("XAX", "에어아시아엑스"),
    # A second sweep, 2026-08-31, taken because the owner noticed names going
    # unshown. 401 aircraft over Seoul, Incheon, Busan and Jeju gave 39
    # designators, of which eight were unnamed - the ones added above and these
    # two. TBJ is TAG Aviation Asia, a business jet operator rather than an
    # airline, and is named anyway because the alternative on the dial is four
    # letters and a number.
    ("KEJ", "카즈에어젯"),
    ("TBJ", "태그에이비에이션"),
    # From the owner's spreadsheet of every carrier serving Korea, passenger
    # and cargo (한국_취항_항공사코드표, 2026-08, sourced from the official
    # Incheon airport listing). Entries already in this table keep their
    # curated names. One correction: the sheet's 3SX for AeroLogic is not a
    # valid ICAO designator - the real code is BOX.
    ("AHK", "에어홍콩"),
    ("AIH", "에어제타"),
    ("AIQ", "타이에어아시아"),
    ("AJX", "에어재팬"),
    ("ALK", "스리랑카항공"),
    ("AMU", "에어마카오"),
    ("AMX", "아에로멕시코"),
    ("ANZ", "에어뉴질랜드"),
    ("APG", "필리핀에어아시아"),
    ("ASA", "알래스카항공"),
    ("AXM", "에어아시아"),
    ("AZG", "실크웨이웨스트"),
    ("BOX", "에어로로직"),
    ("CBJ", "베이징수도항공"),
    ("CDC", "룽에어"),
    ("CEB", "세부퍼시픽"),
    ("CKK", "중국화물항공"),
    ("CQH", "춘추항공"),
    ("CRK", "홍콩항공"),
    ("CSC", "사천항공"),
    ("CSH", "상하이항공"),
    ("CSZ", "심천항공"),
    ("CUA", "중국연합항공"),
    ("CXA", "샤먼항공"),
    ("CYZ", "중국우정항공"),
    ("DHK", "DHL항공"),
    ("ETH", "에티오피아항공"),
    ("GCR", "천진항공"),
    ("GGC", "스카이리스카고"),
    ("HAL", "하와이안항공"),
    ("HGB", "그레이터베이항공"),
    ("HKE", "홍콩익스프레스"),
    ("HLF", "중국센트럴항공"),
    ("HYT", "원통화물항공"),
    ("ICV", "카고룩스이탈리아"),
    ("JDL", "징동화물항공"),
    ("JST", "젯스타"),
    ("LAO", "라오항공"),
    ("LHA", "롱하오항공"),
    ("LOT", "폴란드항공"),
    ("MAA", "마스에어"),
    ("MFX", "센트럼항공"),
    ("MGL", "몽골항공"),
    ("MMA", "미얀마국제항공"),
    ("MNG", "에어로몽골리아"),
    ("MPH", "마틴에어카고"),
    ("MXD", "바틱에어"),
    ("PAC", "폴라에어카고"),
    ("PTA", "파라타항공"),
    ("QDA", "청도항공"),
    ("QNT", "카놋샤크"),
    ("RBA", "로얄브루나이"),
    ("SAS", "스칸디나비아항공"),
    ("SJX", "스타럭스항공"),
    ("SPQ", "썬푸꾸옥항공"),
    ("SWM", "스카이앙코르"),
    ("TAX", "타이에어아시아X"),
    ("TGW", "스쿠트항공"),
    ("TLM", "타이라이언에어"),
    ("TTW", "타이거에어타이완"),
    ("TUA", "투르크멘항공"),
    ("TZP", "집에어"),
    ("UZB", "우즈베키스탄항공"),
    ("VIR", "버진애틀랜틱"),
    ("VSV", "스캇항공"),
    ("WJA", "웨스트젯"),
]


def small_coverage():
    s = io.open(FW + "src/display/UiTextFont.cpp", encoding="utf-8").read()
    seg = s[s.index("SMALL_GLYPHS[] PROGMEM"):s.index("LARGE_GLYPHS[] PROGMEM")]
    return set(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]{4})u,", seg))


def main():
    have = small_coverage()
    seen = {}
    for code, name in AIRLINES:
        assert len(code) == 3 and code.isupper(), code
        for ch in name:
            if ord(ch) not in have:
                seen.setdefault(ch, []).append(code)

    print("  %d airlines in the table" % len(AIRLINES))
    dup = [c for c, _ in AIRLINES if [x for x, _ in AIRLINES].count(c) > 1]
    print("  duplicate codes:", sorted(set(dup)) if dup else "none")
    print()
    print("  syllables the small set is missing (%d):" % len(seen))
    for ch in sorted(seen):
        print("    U+%04X %s  for %s" % (ord(ch), ch, ", ".join(sorted(set(seen[ch])))))
    print()
    print("  bake list:", " ".join("0x%04X" % ord(c) for c in sorted(seen)))
    longest = max(AIRLINES, key=lambda p: len(p[1]))
    print("  longest name: %s (%d syllables)" % (longest[1], len(longest[1])))


main()
