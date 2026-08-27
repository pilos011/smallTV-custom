"""IATA airport code -> the Korean name to show beside the altitude.

The route service answers with an IATA code, three letters that mean nothing at
a glance. This turns the ones that actually appear over Korea into a name.

Names are kept to the shortest natural Korean form, and nothing longer than six
syllables goes in: the destination shares a line with the altitude on a 240 px
dial, and a long one pushes the whole label off the panel. Where a city and its
airport differ the airport wins when a city has more than one (김포 / 인천,
하네다 / 나리타), because that is the distinction worth drawing.

Anything not listed keeps showing its bare IATA code, which is still useful and
cannot be wrong.
"""

AIRPORTS = [
    # Korea
    ("ICN", "인천"),
    ("GMP", "김포"),
    ("CJU", "제주"),
    ("PUS", "김해"),
    ("TAE", "대구"),
    ("KWJ", "광주"),
    ("RSU", "여수"),
    ("USN", "울산"),
    ("CJJ", "청주"),
    ("KUV", "군산"),
    ("WJU", "원주"),
    ("YNY", "양양"),
    ("HIN", "사천"),
    ("MWX", "무안"),
    ("KPO", "포항"),

    # Japan
    ("NRT", "나리타"),
    ("HND", "하네다"),
    ("KIX", "간사이"),
    ("ITM", "이타미"),
    ("CTS", "삿포로"),
    ("FUK", "후쿠오카"),
    ("NGO", "나고야"),
    ("OKA", "오키나와"),
    ("KMQ", "고마쓰"),
    ("TAK", "다카마쓰"),
    ("HIJ", "히로시마"),
    ("KOJ", "가고시마"),
    ("SDJ", "센다이"),
    ("KMJ", "구마모토"),
    ("OIT", "오이타"),
    ("HSG", "사가"),
    ("AOJ", "아오모리"),
    ("KIJ", "니가타"),
    ("TOY", "도야마"),
    ("OKJ", "오카야마"),
    ("UBJ", "야마구치"),
    ("MYJ", "마쓰야마"),
    ("TKS", "도쿠시마"),

    # China
    ("PEK", "베이징"),
    ("PKX", "다싱"),
    ("PVG", "상하이"),
    ("SHA", "훙차오"),
    ("CAN", "광저우"),
    ("SZX", "선전"),
    ("CTU", "청두"),
    ("TFU", "톈푸"),
    ("TAO", "칭다오"),
    ("TSN", "톈진"),
    ("SHE", "선양"),
    ("DLC", "다롄"),
    ("HGH", "항저우"),
    ("NKG", "난징"),
    ("XIY", "시안"),
    ("WUH", "우한"),
    ("HRB", "하얼빈"),
    ("YNJ", "옌지"),
    ("CKG", "충칭"),
    ("KMG", "쿤밍"),
    ("XMN", "샤먼"),
    ("CGO", "정저우"),
    ("CSX", "창사"),
    ("WEH", "웨이하이"),
    ("YNT", "옌타이"),
    ("HET", "후허하오터"),
    ("HAK", "하이커우"),
    ("SYX", "싼야"),

    # Taiwan, Hong Kong, Macau
    ("TPE", "타이베이"),
    ("TSA", "쑹산"),
    ("KHH", "가오슝"),
    ("HKG", "홍콩"),
    ("MFM", "마카오"),

    # Southeast Asia
    ("SIN", "싱가포르"),
    ("BKK", "방콕"),
    ("DMK", "돈므앙"),
    ("HKT", "푸껫"),
    ("CNX", "치앙마이"),
    ("KUL", "쿠알라룸푸르"),
    ("BKI", "코타키나발루"),
    ("CGK", "자카르타"),
    ("DPS", "발리"),
    ("MNL", "마닐라"),
    ("CEB", "세부"),
    ("CRK", "클라크"),
    ("DAD", "다낭"),
    ("SGN", "호치민"),
    ("HAN", "하노이"),
    ("CXR", "나트랑"),
    ("PQC", "푸꾸옥"),
    ("PNH", "프놈펜"),
    ("REP", "씨엠립"),
    ("VTE", "비엔티안"),
    ("RGN", "양곤"),

    # Central and South Asia, Middle East
    ("ULN", "울란바토르"),
    ("TAS", "타슈켄트"),
    ("ALA", "알마티"),
    ("DEL", "델리"),
    ("BOM", "뭄바이"),
    ("DXB", "두바이"),
    ("AUH", "아부다비"),
    ("DOH", "도하"),
    ("IST", "이스탄불"),
    ("TLV", "텔아비브"),

    # Europe
    ("LHR", "런던"),
    ("CDG", "파리"),
    ("FRA", "프랑크푸르트"),
    ("MUC", "뮌헨"),
    ("AMS", "암스테르담"),
    ("ZRH", "취리히"),
    ("VIE", "빈"),
    ("FCO", "로마"),
    ("MXP", "밀라노"),
    ("MAD", "마드리드"),
    ("BCN", "바르셀로나"),
    ("HEL", "헬싱키"),
    ("ARN", "스톡홀름"),
    ("CPH", "코펜하겐"),
    ("OSL", "오슬로"),
    ("WAW", "바르샤바"),
    ("PRG", "프라하"),
    ("BUD", "부다페스트"),
    ("SVO", "모스크바"),
    ("VVO", "블라디보스토크"),
    ("LIS", "리스본"),
    ("DUB", "더블린"),

    # North America
    ("LAX", "로스앤젤레스"),
    ("SFO", "샌프란시스코"),
    ("SEA", "시애틀"),
    ("JFK", "뉴욕"),
    ("EWR", "뉴어크"),
    ("ORD", "시카고"),
    ("ATL", "애틀랜타"),
    ("DFW", "댈러스"),
    ("IAD", "워싱턴"),
    ("BOS", "보스턴"),
    ("IAH", "휴스턴"),
    ("LAS", "라스베이거스"),
    ("SAN", "샌디에이고"),
    ("HNL", "호놀룰루"),
    ("ANC", "앵커리지"),
    ("YVR", "밴쿠버"),
    ("YYZ", "토론토"),
    ("MEX", "멕시코시티"),

    # Found by audit_airports.py, which samples live traffic, asks the route
    # service for both ends of every callsign, and reports the codes this table
    # cannot name. Cheaper and more honest than guessing which airports matter:
    # 156 route ends resolved, and these were the six still showing as letters.
    ("CGQ", "창춘"),
    ("FOC", "푸저우"),
    ("TNA", "지난"),
    ("UKB", "고베"),
    ("NQZ", "아스타나"),
    ("MIA", "마이애미"),
    ("SHI", "미야코지마"),
    ("ISG", "이시가키"),
    ("NGS", "나가사키"),
    ("DAT", "다퉁"),
    ("HFE", "허페이"),

    # Mexico City has two fields and the freighters use NLU - Felipe Angeles,
    # out at Santa Lucia. Both are named for the city rather than the airport,
    # which breaks the rule at the top of this file deliberately: 김포 and 인천
    # are a distinction a Korean reader already holds, and 앙헬레스 is not. On a
    # dial, knowing the aircraft is bound for Mexico is the whole of the point.
    ("NLU", "멕시코시티"),

    # OAK - Oakland, California - is deliberately absent. Korean spells it
    # 오클랜드, and so is Auckland, which is already here as AKL; naming both
    # the same would say something false rather than say nothing. The bare code
    # is at least honestly a code. Give it a name only if a way to tell the two
    # apart at a glance turns up.

    # From the owner's spreadsheet of every airport with a scheduled direct
    # service to Korea (한국_직항_공항코드표, 2026-08, flightconnections.com).
    # Names come from its city column, trimmed to the first segment; entries
    # already in this table keep their curated names. Two edits of note: BWN's
    # eight-syllable city name became 브루나이, and the sheet's BSZ for Bishkek
    # was replaced by FRU - Manas International's actual IATA code - since a
    # wrong code labelled with a right name is still a wrong entry.
    ("KTM", "카트만두"),
    ("RMQ", "타이중"),
    ("UBN", "울란바토르"),
    ("DTW", "디트로이트"),
    ("MSP", "미니애폴리스"),
    ("SLC", "솔트레이크시티"),
    ("DAC", "다카"),
    ("DLI", "달랏"),
    ("BWN", "브루나이"),
    ("CMB", "콜롬보"),
    ("ADD", "아디스아바바"),
    ("MDC", "마나도"),
    ("BTH", "바탐"),
    ("KCZ", "고치"),
    ("KKJ", "기타큐슈"),
    ("KMI", "미야자키"),
    ("FSZ", "시즈오카"),
    ("AXT", "아키타"),
    ("YGJ", "요나고"),
    ("IBR", "이바라키"),
    ("NGB", "닝보"),
    ("MDG", "무단장"),
    ("SJW", "스자좡"),
    ("YTY", "양저우"),
    ("YNZ", "옌청"),
    ("DSN", "오르도스"),
    ("URC", "우루무치"),
    ("WUX", "우시"),
    ("JMU", "자무쓰"),
    ("DYG", "장자제"),
    ("JJN", "취안저우"),
    ("CIT", "심켄트"),
    ("KTI", "프놈펜"),
    ("YUL", "몬트리올"),
    ("YYC", "캘거리"),
    ("ZAG", "자그레브"),
    ("FRU", "비슈케크"),
    ("KBV", "끄라비"),
    ("ASB", "아시가바트"),
    ("WRO", "브로츠와프"),
    ("MPH", "보라카이"),
    ("TAG", "보홀"),
    ("KLO", "칼리보"),

    # Freight hubs. FedEx and UPS pass over Korea constantly and their
    # destinations are not places passenger traffic goes.
    ("MEM", "멤피스"),
    ("SDF", "루이빌"),
    ("CGN", "쾰른"),
    ("LEJ", "라이프치히"),
    ("LGG", "리에주"),
    ("OTP", "부쿠레슈티"),

    # Pacific and Oceania
    ("GUM", "괌"),
    ("SPN", "사이판"),
    ("SYD", "시드니"),
    ("MEL", "멜버른"),
    ("BNE", "브리즈번"),
    ("AKL", "오클랜드"),
    ("NAN", "나디"),
]
