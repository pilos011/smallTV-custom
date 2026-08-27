"""ICAO type designator -> the rotorcraft name to show.

A helicopter almost never carries an airline designator, so the line that names
the carrier for an airliner names the machine instead. The feed gives the ICAO
type code directly; this turns the ones a reader would recognise into a name.

Kept to what actually flies over Korea - police, fire, medical, news, military -
plus the civil types common enough to be worth naming. Anything not listed keeps
showing its bare type code, which is still useful and cannot be wrong.
"""

ROTORCRAFT = [
    # Military and government, which is most of what is up over Seoul
    ("H60", "블랙호크"),
    ("S70", "블랙호크"),
    ("SH60", "시호크"),
    ("KUH1", "수리온"),
    ("CH47", "치누크"),
    ("UH1", "휴이"),
    ("AH64", "아파치"),
    ("AH1", "코브라"),
    ("LYNX", "링스"),
    ("MI8", "미-8"),
    ("MI17", "미-17"),
    ("H500", "휴즈500"),
    ("MD50", "휴즈500"),

    # Airbus Helicopters, formerly Eurocopter.
    #
    # Airbus renamed this whole range in 2015 - EC135 became H135, EC145 became
    # H145, and so on - while the ICAO designators stayed as they were. So the
    # EC** code and the H*** code are the same machine arriving under two
    # spellings, and both are named for what is painted on it today. EC120 and
    # BK117 keep their old names: production ended before the rename, so no
    # H-number was ever put on one.
    ("EC20", "유로콥터120"),
    ("EC30", "에어버스H130"),
    ("H130", "에어버스H130"),
    ("EC35", "에어버스H135"),
    ("H135", "에어버스H135"),
    ("EC45", "에어버스H145"),
    ("H145", "에어버스H145"),
    ("EC55", "에어버스H155"),
    ("H155", "에어버스H155"),
    # AS50 is the designator for the whole AS350 Ecureuil family, and the model
    # still in production is sold as the H125 - Airbus renamed the AS350 B3e in
    # 2015 and the ICAO code did not follow. So a current machine arrives as
    # AS50 and has to be named H125, not 350. Naming it "에어버스350" was also
    # asking to be misread as the A350 airliner, on a dial full of airliners.
    ("AS50", "에어버스H125"),
    ("H125", "에어버스H125"),
    ("H160", "에어버스H160"),
    ("H175", "에어버스H175"),
    ("AS65", "돌핀"),
    ("BK17", "유로콥터117"),

    # Leonardo, formerly AgustaWestland
    ("A109", "아구스타109"),
    ("A139", "아구스타139"),
    ("A169", "아구스타169"),
    ("A189", "아구스타189"),
    ("EH10", "멀린"),

    # Bell
    ("B06", "벨206"),
    ("B407", "벨407"),
    ("B412", "벨412"),
    ("B429", "벨429"),
    ("B505", "벨505"),
    ("B430", "벨430"),

    # Sikorsky and Robinson
    ("S76", "시코르스키76"),
    ("S92", "시코르스키92"),
    ("R22", "로빈슨22"),
    ("R44", "로빈슨44"),
    ("R66", "로빈슨66"),
]
