#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from typing import Optional
from unittest.mock import MagicMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import prefix_mgr
from openr.cli.tests import helpers

from .fixtures import (
    ADVERTISED_ROUTES_OUTPUT,
    ADVERTISED_ROUTES_OUTPUT_DETAILED,
    ADVERTISED_ROUTES_OUTPUT_JSON,
    MOCKED_ADVERTISED_ROUTES,
)


BASE_MODULE = "openr.cli.clis.prefix_mgr"
BASE_CMD_MODULE = "openr.cli.commands.prefix_mgr"


class CliPrefixManagerTests(TestCase):
    maxDiff: Optional[int] = None

    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrCli.prefixmgr,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    @patch(f"{BASE_CMD_MODULE}.PrefixMgrCmd._get_config")
    def test_prefixmgr_advertised_routes(
        self, mocked_openr_config: MagicMock, mocked_openr_client: MagicMock
    ) -> None:
        # Set mock data for testing
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getAdvertisedRoutesFiltered.return_value = (
            MOCKED_ADVERTISED_ROUTES
        )

        tag_map = {
            "NOT_USED_TAG_NAME": {"tagSet": ["not_used_tag"]},
            "TAG_NAME2": {"tagSet": ["65520:822"]},
        }

        mocked_openr_config.return_value = {
            "area_policies": {"definitions": {"openrTag": {"objects": tag_map}}}
        }

        # Invoke with no flags & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--no-detail", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT, invoked_return.stdout)

        # Invoke with [--detail] & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--detail", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT_DETAILED, invoked_return.stdout)

        # Invoke with [--json] & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--json", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT_JSON, invoked_return.stdout)
