"""Regression tests for the architecture boundary checker."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
TOOLS = REPOSITORY / "tools"
sys.path.insert(0, str(TOOLS))
import check_architecture  # noqa: E402


SIGNALS_CONTRACTS = """#pragma once
#include <cstdint>
#include <optional>
#include "vehicle_core/telemetry_contracts.hpp"
// Portable contracts: no Mazda, CAN, RTOS, or driver dependency.
namespace vehicle_signals {
using vehicle_core::ValidationStatus;
class SignalId {
public:
  constexpr SignalId() noexcept = default;
  constexpr explicit SignalId(std::uint16_t value) noexcept : value_(value) {}
  constexpr bool valid() const noexcept { return value_ != 0; }
  friend constexpr bool operator==(SignalId left, SignalId right) noexcept {
    return left.value_ == right.value_;
  }
private:
  std::uint16_t value_{0};
};
enum class SignalType : std::uint8_t { Boolean, Number, Enum };
enum class SignalUnit : std::uint8_t { None };
enum class SignalCapability : std::uint8_t { Read = 1 };
struct SignalReading { std::optional<float> value{}; };
}
"""

SIGNALS_CATALOG = """#pragma once
#include <cstddef>
#include <string_view>
#include "vehicle_signals/signal_contracts.hpp"
namespace vehicle_signals {
struct SignalMetadata {
  SignalId id{};
  std::string_view key{};
  SignalType type{SignalType::Boolean};
  SignalUnit unit{SignalUnit::None};
  ValidationStatus validation{ValidationStatus::Reference};
  SignalCapability capabilities{SignalCapability::Read};
};
class SignalCatalogView {
public:
  template <std::size_t N>
  constexpr SignalCatalogView(const SignalMetadata (&entries)[N]) noexcept
      : entries_(entries), count_(N) {}
  constexpr bool well_formed() const noexcept { return count_ != 0; }
  constexpr const SignalMetadata *find(SignalId id) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) { if (entries_[i].id == id) return &entries_[i]; }
    return nullptr;
  }
  constexpr const SignalMetadata *find(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) { if (entries_[i].key == key) return &entries_[i]; }
    return nullptr;
  }
private:
  const SignalMetadata *entries_{nullptr};
  std::size_t count_{0};
};
}
"""

SIGNALS_PROVIDER = """#pragma once
#include "vehicle_signals/signal_catalog.hpp"
namespace vehicle_signals {
class SignalProvider {
public:
  virtual SignalCatalogView catalog() const noexcept = 0;
protected:
  ~SignalProvider() = default;
};
}
"""


def write_signals_fixture(root: Path) -> None:
    """Write a self-contained core + vehicle_signals + Mazda fixture tree."""

    core = root / "third_party/esp32-vehicle-can-core/components/vehicle_core"
    core_include = core / "include/vehicle_core"
    core_include.mkdir(parents=True)
    (core_include / "telemetry_contracts.hpp").write_text(
        "#pragma once\n"
        "namespace vehicle_core { enum class ValidationStatus { Reference }; }\n",
        encoding="utf-8",
    )
    (core_include / "signal.hpp").write_text(
        "#pragma once\n#include \"vehicle_core/telemetry_contracts.hpp\"\n"
        "namespace vehicle_core { template <typename T> class Signal {}; }\n",
        encoding="utf-8",
    )
    (core / "CMakeLists.txt").write_text(
        "add_library(vehicle_core INTERFACE)\n"
        "target_include_directories(vehicle_core INTERFACE\n"
        "  ${CMAKE_CURRENT_SOURCE_DIR}/include)\n",
        encoding="utf-8",
    )
    mazda = root / "lib/mazda/include/mazda"
    mazda.mkdir(parents=True)
    (mazda / "types.hpp").write_text(
        "#pragma once\nnamespace mazda { enum class TurnState { Off, Left }; }\n",
        encoding="utf-8",
    )
    signals = root / "lib/vehicle_signals"
    (signals / "include/vehicle_signals").mkdir(parents=True)
    (signals / "include/vehicle_signals/signal_contracts.hpp").write_text(
        SIGNALS_CONTRACTS, encoding="utf-8"
    )
    (signals / "include/vehicle_signals/signal_catalog.hpp").write_text(
        SIGNALS_CATALOG, encoding="utf-8"
    )
    (signals / "include/vehicle_signals/signal_provider.hpp").write_text(
        SIGNALS_PROVIDER, encoding="utf-8"
    )
    (signals / "CMakeLists.txt").write_text(
        "# Portable signals; no Mazda component is reachable from this target.\n"
        "add_library(vehicle_signals INTERFACE)\n"
        "target_include_directories(vehicle_signals INTERFACE\n"
        "  ${CMAKE_CURRENT_SOURCE_DIR}/include)\n"
        "target_link_libraries(vehicle_signals INTERFACE vehicle_core)\n",
        encoding="utf-8",
    )


def check_signals_fixture(root: Path) -> str:
    work_dir = root / "work"
    work_dir.mkdir()
    output = io.StringIO()
    with redirect_stdout(output), redirect_stderr(io.StringIO()):
        check_architecture._check_vehicle_signals_only(root, "cmake", ("c++",), work_dir)
    return output.getvalue()


def write_action_engine_fixture(root: Path) -> None:
    """Copy the real vehicle_signals and action_engine over a stub core."""

    core = root / "third_party/esp32-vehicle-can-core/components/vehicle_core"
    core_include = core / "include/vehicle_core"
    core_include.mkdir(parents=True)
    (core_include / "telemetry_contracts.hpp").write_text(
        "#pragma once\n#include <cstdint>\n"
        "namespace vehicle_core {\n"
        "enum class Availability : std::uint8_t {\n"
        "  NoData, Fresh, Stale, FreshnessUnverified, Unavailable };\n"
        "enum class ValidationStatus : std::uint8_t { Reference, Observed, Confirmed };\n"
        "}\n",
        encoding="utf-8",
    )
    (core_include / "signal.hpp").write_text(
        "#pragma once\nnamespace vehicle_core { template <typename T> class Signal {}; }\n",
        encoding="utf-8",
    )
    (core / "CMakeLists.txt").write_text(
        "add_library(vehicle_core INTERFACE)\n"
        "target_include_directories(vehicle_core INTERFACE\n"
        "  ${CMAKE_CURRENT_SOURCE_DIR}/include)\n",
        encoding="utf-8",
    )
    mazda = root / "lib/mazda/include/mazda"
    mazda.mkdir(parents=True)
    (mazda / "types.hpp").write_text(
        "#pragma once\nnamespace mazda { enum class TurnState { Off, Left }; }\n",
        encoding="utf-8",
    )
    for library in ("vehicle_signals", "action_engine"):
        shutil.copytree(REPOSITORY / "lib" / library, root / "lib" / library)


def check_action_engine_fixture(root: Path) -> str:
    work_dir = root / "work"
    work_dir.mkdir()
    output = io.StringIO()
    with redirect_stdout(output), redirect_stderr(io.StringIO()):
        check_architecture._check_action_engine_only(root, "cmake", ("c++",), work_dir)
    return output.getvalue()


def prepend(path: Path, text: str) -> None:
    content = path.read_text(encoding="utf-8").replace("#pragma once\n", "", 1)
    path.write_text("#pragma once\n" + text + content, encoding="utf-8")


def write_consumer_fixture(root: Path) -> None:
    host = root / "tests/host"
    host.mkdir(parents=True)
    (host / "generic_signal_consumer.hpp").write_text(
        "#pragma once\n#include <optional>\n"
        '#include "mazda/signal_provider.hpp"\n'
        '#include "vehicle_signals/signal_contracts.hpp"\n'
        '// #include "mazda/vehicle_telemetry.hpp" is only mentioned in a comment.\n',
        encoding="utf-8",
    )
    (host / "generic_signal_consumer.cpp").write_text(
        '#include "generic_signal_consumer.hpp"\n', encoding="utf-8"
    )
    (host / "CMakeLists.txt").write_text(
        "add_library(generic_signal_consumer STATIC generic_signal_consumer.cpp)\n"
        "target_link_libraries(generic_signal_consumer PUBLIC mazda_telemetry_contracts)\n",
        encoding="utf-8",
    )


class ArchitectureCheckerRegressionTests(unittest.TestCase):
    def test_clean_generic_consumer_passes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-consumer-fixture-") as directory:
            root = Path(directory)
            write_consumer_fixture(root)
            output = io.StringIO()
            with redirect_stdout(output):
                check_architecture._check_generic_consumer(root)
        self.assertIn("OK   generic signal consumer", output.getvalue())

    def test_generic_consumer_private_access_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-consumer-fixture-") as directory:
            root = Path(directory)
            write_consumer_fixture(root)
            source = root / "tests/host/generic_signal_consumer.cpp"
            source.write_text(
                source.read_text(encoding="utf-8")
                + '#include "mazda/vehicle_telemetry_service.hpp"\n'
                + "#include <mazda/types.hpp>\n",
                encoding="utf-8",
            )
            cmake = root / "tests/host/CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "target_include_directories(generic_signal_consumer PRIVATE private_include)\n"
                + "target_link_libraries(generic_signal_consumer PRIVATE mazda_telemetry)\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_architecture._check_generic_consumer(root)
        detail = str(raised.exception)
        self.assertIn("includes forbidden header mazda/vehicle_telemetry_service.hpp", detail)
        self.assertIn("includes forbidden header mazda/types.hpp", detail)
        self.assertIn("must not add include directories", detail)
        self.assertIn("links forbidden target mazda_telemetry", detail)
        self.assertNotIn("mazda/vehicle_telemetry.hpp", detail)

    def test_clean_vehicle_signals_fixture_passes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            write_signals_fixture(root)
            output = check_signals_fixture(root)
        self.assertIn("OK   vehicle_signals builds without Mazda/RTOS dependencies", output)

    def test_vehicle_signals_header_reaching_mazda_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            write_signals_fixture(root)
            signals = root / "lib/vehicle_signals"
            cmake = signals / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "target_include_directories(vehicle_signals INTERFACE\n"
                + "  ${CMAKE_CURRENT_SOURCE_DIR}/../mazda/include)\n",
                encoding="utf-8",
            )
            prepend(
                signals / "include/vehicle_signals/signal_catalog.hpp",
                '#include "mazda/types.hpp"\n',
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_signals_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden vehicle_signals target include directory", detail)
        self.assertIn("forbidden vehicle_signals consumer include directory", detail)
        self.assertIn("references Mazda outside comments", detail)
        self.assertIn("forbidden vehicle_signals dependency", detail)
        self.assertIn("lib/mazda/include/mazda/types.hpp", detail)

    def test_vehicle_signals_header_reaching_mutable_core_signal_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            write_signals_fixture(root)
            # The core include root is legitimately exported; only the
            # compiler's dependency closure can see this violation.
            prepend(
                root / "lib/vehicle_signals/include/vehicle_signals/signal_contracts.hpp",
                '#include "vehicle_core/signal.hpp"\n',
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_signals_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden vehicle_signals core dependency", detail)
        self.assertIn("vehicle_core/signal.hpp", detail)

    def test_vehicle_signals_mazda_link_target_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            write_signals_fixture(root)
            cmake = root / "lib/vehicle_signals/CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "target_link_libraries(vehicle_signals INTERFACE mazda_telemetry)\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_signals_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden vehicle_signals target link target: mazda_telemetry", detail)

    def test_vehicle_signals_mazda_key_and_type_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            write_signals_fixture(root)
            catalog = root / "lib/vehicle_signals/include/vehicle_signals/signal_catalog.hpp"
            catalog.write_text(
                catalog.read_text(encoding="utf-8")
                + "namespace vehicle_signals {\n"
                + "enum class TurnState { Off };\n"
                + 'inline constexpr std::string_view kTurnKey{"vehicle.turn_state"};\n'
                + "}\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_signals_fixture(root)
        detail = str(raised.exception)
        self.assertIn("names Mazda type TurnState", detail)
        self.assertIn("Mazda catalog key literal: vehicle.turn_state", detail)

    def test_clean_action_engine_passes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-engine-fixture-") as directory:
            root = Path(directory)
            write_action_engine_fixture(root)
            output = check_action_engine_fixture(root)
        self.assertIn("OK   action_engine builds on vehicle_signals alone", output)

    def test_action_engine_header_reaching_mazda_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-engine-fixture-") as directory:
            root = Path(directory)
            write_action_engine_fixture(root)
            engine = root / "lib/action_engine"
            cmake = engine / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "target_include_directories(action_engine PUBLIC\n"
                + "  ${CMAKE_CURRENT_SOURCE_DIR}/../mazda/include)\n",
                encoding="utf-8",
            )
            prepend(engine / "include/action_engine/action.hpp", '#include "mazda/types.hpp"\n')
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_action_engine_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden action_engine target include directory", detail)
        self.assertIn("forbidden action_engine consumer include directory", detail)
        self.assertIn("forbidden action_engine target (condition.cpp) include directory", detail)
        self.assertIn("action.hpp references Mazda outside comments", detail)
        self.assertIn("forbidden action_engine dependency", detail)
        self.assertIn("lib/mazda/include/mazda/types.hpp", detail)

    def test_action_engine_source_reaching_mutable_core_signal_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-engine-fixture-") as directory:
            root = Path(directory)
            write_action_engine_fixture(root)
            # Only the engine's own compile command reaches this header, so
            # only the target dependency closure can see the violation.
            source = root / "lib/action_engine/src/engine.cpp"
            source.write_text(
                '#include "vehicle_core/signal.hpp"\n' + source.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_action_engine_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden action_engine core dependency", detail)
        self.assertIn("vehicle_core/signal.hpp", detail)

    def test_action_engine_link_to_output_or_mazda_target_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-engine-fixture-") as directory:
            root = Path(directory)
            write_action_engine_fixture(root)
            cmake = root / "lib/action_engine/CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "target_link_libraries(action_engine PUBLIC mazda_telemetry local_argb)\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_action_engine_fixture(root)
        detail = str(raised.exception)
        self.assertIn("forbidden action_engine target link target: mazda_telemetry", detail)
        self.assertIn("forbidden action_engine target link target: local_argb", detail)

    def test_action_engine_mazda_keys_types_and_output_tokens_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-engine-fixture-") as directory:
            root = Path(directory)
            write_action_engine_fixture(root)
            source = root / "lib/action_engine/src/rules.cpp"
            source.write_text(
                source.read_text(encoding="utf-8")
                + "// wled, argb and can_bus in a comment are fine.\n"
                + "namespace action_engine {\n"
                + "enum class TurnState { Off };\n"
                + 'inline constexpr const char *kTurnKey = "vehicle.turn_state";\n'
                + "inline constexpr int wled_port = 21324;\n"
                + "inline constexpr int twai_mode = 0;\n"
                + "inline constexpr int argb_pixels = 100;\n"
                + "inline constexpr int can_bus_rate = 500;\n"
                + "inline constexpr int freertos_ticks = 1;\n"
                + "}\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_action_engine_fixture(root)
        detail = str(raised.exception)
        self.assertIn("rules.cpp names Mazda type TurnState", detail)
        self.assertIn("Mazda catalog key literal: vehicle.turn_state", detail)
        for token in ("wled", "twai", "argb", "can_bus", "freertos"):
            self.assertIn(f"rules.cpp references forbidden {token} outside comments", detail)

    def test_vehicle_core_target_private_mazda_dependency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-core-fixture-") as directory:
            root = Path(directory)
            core = root / "third_party/esp32-vehicle-can-core/components/vehicle_core"
            include = core / "include/vehicle_core"
            source_dir = core / "src"
            include.mkdir(parents=True)
            source_dir.mkdir()
            (include / "vehicle_core.hpp").write_text(
                "#pragma once\n"
                "namespace vehicle_core {\n"
                "struct RawCanFrame { bool is_valid() const { return true; } };\n"
                "inline bool library_is_available() { return true; }\n"
                "}\n",
                encoding="utf-8",
            )
            (include / "reading.hpp").write_text(
                "#pragma once\n"
                "namespace vehicle_core { template <typename T> struct Reading { T value{}; }; }\n",
                encoding="utf-8",
            )
            (include / "notification.hpp").write_text(
                "#pragma once\n#include \"vehicle_core/reading.hpp\"\n"
                "namespace vehicle_core { template <typename T> struct Notification { Reading<T> current{}; }; }\n",
                encoding="utf-8",
            )
            (include / "telemetry_contracts.hpp").write_text(
                "#pragma once\n#include \"vehicle_core/reading.hpp\"\n"
                "#include \"vehicle_core/notification.hpp\"\n",
                encoding="utf-8",
            )
            (root / "lib/mazda/include/mazda").mkdir(parents=True)
            (root / "lib/mazda/include/mazda/definitions.hpp").write_text(
                "#pragma once\nnamespace mazda { struct Definition {}; }\n",
                encoding="utf-8",
            )
            source = source_dir / "vehicle_core.cpp"
            source.write_text(
                '#include "mazda/definitions.hpp"\n'
                '#include "vehicle_core/vehicle_core.hpp"\n',
                encoding="utf-8",
            )
            cmake = core / "CMakeLists.txt"
            cmake.write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(vehicle_core_fixture LANGUAGES CXX)\n"
                "add_library(vehicle_core STATIC src/vehicle_core.cpp)\n"
                "target_include_directories(vehicle_core PUBLIC include PRIVATE "
                f"{root / 'lib/mazda/include'})\n",
                encoding="utf-8",
            )

            work_dir = root / "work"
            work_dir.mkdir()
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    check_architecture._check_core_only(
                        root,
                        "cmake",
                        ("c++",),
                        work_dir,
                    )

            detail = str(raised.exception)
            self.assertIn("vehicle_core target", detail)
            self.assertIn("lib/mazda", detail)

    def test_active_cmake_and_yaml_build_files_reject_retired_capture_marker(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-capture-fixture-") as directory:
            root = Path(directory)
            files = (
                root / "CMakeLists.txt",
                root / "components/example/CMakeLists.txt",
                root / "firmware/example/CMakeLists.txt",
                root / "components/example/idf_component.yaml",
            )
            marker = "raw_" + "capture"
            for path in files:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"set(RETIRED_DEPENDENCY {marker})\n", encoding="utf-8")

            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_architecture._check_capture_removal(root)

            detail = str(raised.exception)
            for path in files:
                self.assertIn(path.relative_to(root).as_posix(), detail)


if __name__ == "__main__":
    unittest.main()
