# Account index persistence tests

Run the standalone Debug target with the installed Qt 6 Core package:

```sh
cmake -S Telegram/SourceFiles/tests/account_index -B /tmp/account-index-tests \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build /tmp/account-index-tests
ctest --test-dir /tmp/account-index-tests --output-on-failure
```

The tests exercise the production account-index reader, writer and free-index allocator. They cover seven entries in the existing binary format, deletion and re-addition, 500 synthetic entries saved to a temporary file, legacy active-account fallback, malformed input and integer boundaries. They do not open user account storage, authenticate accounts, exercise the encrypted storage envelope, or establish 500 network connections. Window rendering and real-account restoration require separate application validation.
