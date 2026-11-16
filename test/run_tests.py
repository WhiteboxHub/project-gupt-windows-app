"""
Master End-to-End Test Runner for Gupt Remote Desktop.
Runs all unit and integration tests, measures latencies, and reports status.
"""
import unittest
import sys
import os

# Add test folder to sys.path
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

def main():
    print("=" * 70)
    print(" GUPT REMOTE DESKTOP - END-TO-END TEST SUITE")
    print("=" * 70)

    loader = unittest.TestLoader()
    suite = loader.discover(start_dir=TEST_DIR, pattern="test_*.py")

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    print("=" * 70)
    print(f" Tests Run: {result.testsRun}")
    print(f" Passed:    {result.testsRun - len(result.failures) - len(result.errors)}")
    print(f" Failures:  {len(result.failures)}")
    print(f" Errors:    {len(result.errors)}")
    print("=" * 70)

    if result.wasSuccessful():
        print(" >>> ALL GUPT END-TO-END TESTS PASSED SUCCESSFULLY! <<<")
        sys.exit(0)
    else:
        print(" >>> SOME TESTS FAILED. See details above. <<<")
        sys.exit(1)

if __name__ == '__main__':
    main()
