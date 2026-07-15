from cardputer_probe.cli import parser


def test_cli_defaults_are_non_destructive():
    args = parser().parse_args([])
    assert args.target == "all"
    assert args.write_test is False
    assert args.report.name == "probe-report.json"


def test_cli_supports_saved_local_dyson_probe():
    args = parser().parse_args(["dyson-local", "--write-test"])
    assert args.target == "dyson-local"
    assert args.write_test is True
