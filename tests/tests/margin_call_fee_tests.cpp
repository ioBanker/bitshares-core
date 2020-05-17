/*
 * Copyright (c) 2020 Contributors
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <boost/test/unit_test.hpp>

#include <graphene/chain/hardfork.hpp>

#include "../common/database_fixture.hpp"

// For account history
#include <vector>
#include <graphene/app/api.hpp>


using namespace graphene::chain;
using namespace graphene::chain::test;

struct bitasset_database_fixture : database_fixture {
   bitasset_database_fixture()
           : database_fixture() {
   }

   const limit_order_create_operation
   create_sell_operation(account_id_type user, const asset &amount, const asset &recv) {
      const time_point_sec order_expiration = time_point_sec::maximum();
      const price &fee_core_exchange_rate = price::unit_price();
      limit_order_create_operation op = create_sell_operation(user, amount, recv, order_expiration,
                                                              fee_core_exchange_rate);
      return op;
   }

   const limit_order_create_operation
   create_sell_operation(account_id_type user, const asset &amount, const asset &recv,
                         const time_point_sec order_expiration,
                         const price &fee_core_exchange_rate) {
      limit_order_create_operation op = create_sell_operation(user(db), amount, recv, order_expiration,
                                                              fee_core_exchange_rate);
      return op;
   }

   const limit_order_create_operation
   create_sell_operation(const account_object &user, const asset &amount, const asset &recv,
                         const time_point_sec order_expiration,
                         const price &fee_core_exchange_rate) {
      limit_order_create_operation sell_order;
      sell_order.seller = user.id;
      sell_order.amount_to_sell = amount;
      sell_order.min_to_receive = recv;
      sell_order.expiration = order_expiration;

      return sell_order;
   }

   const asset_create_operation create_user_issued_asset_operation(const string &name, const account_object &issuer,
                                                                   uint16_t flags, const price &core_exchange_rate,
                                                                   uint8_t precision, uint16_t maker_fee_percent,
                                                                   uint16_t taker_fee_percent) {
      asset_create_operation creator;
      creator.issuer = issuer.id;
      creator.fee = asset();
      creator.symbol = name;
      creator.common_options.max_supply = 0;
      creator.precision = precision;

      creator.common_options.core_exchange_rate = core_exchange_rate;
      creator.common_options.max_supply = GRAPHENE_MAX_SHARE_SUPPLY;
      creator.common_options.flags = flags;
      creator.common_options.issuer_permissions = flags;
      creator.common_options.market_fee_percent = maker_fee_percent;
      creator.common_options.extensions.value.taker_fee_percent = taker_fee_percent;

      return creator;

   }
};


BOOST_FIXTURE_TEST_SUITE(margin_call_fee_tests, bitasset_database_fixture)

   /**
    * Test a simple scenario of a Complete Fill of a Call Order as a Maker after HF
    *
    * 0. Advance to HF
    * 1. Initialize actors, a smart asset called SMARTBIT
    * 2. Publish feed
    * 3. (Order 1: Call order) Bob borrows a **"small"** amount of SMARTBIT into existence.
    *     Bob retains the asset in his own balances, or transfers it, or sells it is not critical
    *     because his debt position is what will be tracked.
    * 4. The feed price is updated to indicate that the collateral drops enough to trigger a margin call
    *    **but not enough** to trigger a global settlement.
    *    Bob's activated margin call cannot be matched against any existing limit order's price.
    * 5. (Order 2: Limit order) Alice places a **"large"** limit order to sell SMARTBIT at a price
    *    that will overlap with Bob's "activated" call order / margin call.
    *    **Bob should be charged as a maker, and Alice as a taker.**
    *    Alice's limit order should be paritally filled, but Bob's order should be completely filled,
    *    and the debt position should be closed.
    */
   BOOST_AUTO_TEST_CASE(complete_fill_of_call_order_as_maker) {
      try {
         //////
         // 0. Advance to activate hardfork
         //////
         BOOST_TEST_MESSAGE("Advancing past Hardfork BSIP74");
         generate_blocks(HARDFORK_CORE_BSIP74_TIME);
         generate_block();
         set_expiration(db, trx);


         //////
         // 1. Initialize actors, a UIA called JCOIN, a smart asset called SMARTBIT
         //////
         // Initialize for the current time
         trx.clear();
         set_expiration(db, trx);

         // Initialize actors
         ACTORS((alice)(bob));
         ACTORS((smartissuer)(feedproducer));

         // Initialize tokens
         // CORE asset exists by default
         const asset_object &core = asset_id_type()(db);
         const asset_id_type core_id = core.id;
         const int64_t CORE_UNIT = asset::scaled_precision(core.precision).value; // 100000 satoshi CORE in 1 CORE

         // Create the SMARTBIT asset
         const int16_t SMARTBIT_UNIT = 10000; // 10000 satoshi SMARTBIT in 1 SMARTBIT
         const uint16_t smartbit_market_fee_percent = 2 * GRAPHENE_1_PERCENT;
         const uint16_t smartbit_margin_call_fee_ratio = 50; // 5% expressed in terms of GRAPHENE_COLLATERAL_RATIO_DENOM
         // Define the margin call fee ratio
         create_bitasset("SMARTBIT", smartissuer.id, smartbit_market_fee_percent, charge_market_fee, 4, core_id,
                         GRAPHENE_MAX_SHARE_SUPPLY, {}, smartbit_margin_call_fee_ratio);
         // Obtain asset object after a block is generated to obtain the final object that is commited to the database
         generate_block();
         const asset_object smartbit = get_asset("SMARTBIT");
         const asset_id_type smartbit_id = smartbit.id;
         update_feed_producers(smartbit, {feedproducer.id});

         // Initialize token balance of actors
         // Alice should start with 5,000,000 CORE
         const asset alice_initial_core = asset(5000000 * CORE_UNIT);
         transfer(committee_account, alice.id, alice_initial_core);
         BOOST_REQUIRE_EQUAL(get_balance(alice_id, core_id), alice_initial_core.amount.value);

         // Bob should start with enough CORE to back 200 SMARTBIT subject to
         // (a) to an initial price feed of 1 satoshi SMARTBIT for 20 satoshi CORE
         // = 0.0001 SMARTBIT for 0.00020 CORE = 1 SMARTBIT for 2 CORE
         // (b) an initial collateral ratio of 2x
         const price initial_feed_price =
                 smartbit.amount(1) / core.amount(20); // 1 satoshi SMARTBIT for 20 satoshi CORE
         const asset bob_initial_smart = smartbit.amount(200 * SMARTBIT_UNIT); // 2,000,000 satoshi SMARTBIT
         const asset bob_initial_core = core.amount(
                 2 * (bob_initial_smart * initial_feed_price).amount); // 80,000,000 satoshi CORE
         transfer(committee_account, bob.id, bob_initial_core);
         BOOST_REQUIRE_EQUAL(get_balance(bob, core), 80000000);


         //////
         // 2. Publish feed
         //////
         price_feed current_feed;
         current_feed.settlement_price = initial_feed_price;
         current_feed.maintenance_collateral_ratio = 1750; // MCR of 1.75x
         publish_feed(smartbit, feedproducer, current_feed);
         FC_ASSERT(smartbit.bitasset_data(db).current_feed.settlement_price == current_feed.settlement_price);


         //////
         // 3. (Order 1: Call order) Bob borrows a **"small"** amount of SMARTBIT into existence.
         //    Bob retains the asset in his own balances, or transfers it, or sells it is not critical
         //    because his debt position is what will be tracked.
         //////
         call_order_id_type bob_call_id = (*borrow(bob, bob_initial_smart, bob_initial_core)).id;
         BOOST_REQUIRE_EQUAL(get_balance(bob, smartbit), 200 * SMARTBIT_UNIT);
         BOOST_CHECK(!smartbit.bitasset_data(db).has_settlement()); // No global settlement
         const price bob_initial_cr = bob_call_id(db).collateralization(); // Units of collateral / debt
         BOOST_CHECK_EQUAL(bob_initial_cr.base.amount.value, 80000000); // Collateral of 80,000,000 satoshi CORE
         BOOST_CHECK_EQUAL(bob_initial_cr.quote.amount.value, 2000000); // Debt of 2,000,000 satoshi SMARTBIT


         //////
         // 4. The feed price is updated to indicate that the collateral drops enough to trigger a margin call
         //    **but not enough** to trigger a global settlement.
         //    Bob's activated margin call cannot be matched against any existing limit order's price.
         //////
         // Adjust the price such that the initial CR of Bob's position (CR_0) drops to 1.7x = (17/10)x
         // Want new price = 1.7 / CR_0 = (17/10) / CR_0
         //
         // Collateral ratios are defined as collateral / debt
         // BitShares prices are conventionally defined as debt / collateral
         // The new price can be expressed with the available codebase as
         // = (17/10) * ~CR_0 = ~CR_0 * (17/10)
         const price intermediate_feed_price = ~bob_initial_cr * ratio_type(17, 10); // Units of debt / collateral
         // Reduces to (2000000 * 17) / (80000000 * 10) = (17) / (40 * 10) = 17 / 400
         BOOST_CHECK(intermediate_feed_price < initial_feed_price);
         BOOST_CHECK_EQUAL(intermediate_feed_price.base.amount.value, 17); // satoshi SMARTBIT
         BOOST_CHECK_EQUAL(intermediate_feed_price.quote.amount.value, 400); // satoshi CORE

         current_feed.settlement_price = intermediate_feed_price;
         publish_feed(smartbit, feedproducer, current_feed);

         BOOST_CHECK(smartbit.bitasset_data(db).current_feed.settlement_price == current_feed.settlement_price);
         BOOST_CHECK(!smartbit.bitasset_data(db).has_settlement()); // No global settlement

         // Check Bob's debt to the blockchain
         BOOST_CHECK_EQUAL(bob_call_id(db).debt.value, bob_initial_smart.amount.value);
         BOOST_CHECK_EQUAL(bob_call_id(db).collateral.value, bob_initial_core.amount.value);

         // Check Bob's balances
         BOOST_CHECK_EQUAL(get_balance(bob_id(db), smartbit_id(db)), bob_initial_smart.amount.value);
         BOOST_CHECK_EQUAL(get_balance(bob_id(db), core_id(db)), 0);



         //////
         // 5. (Order 2: Limit order) Alice places a **"large"** limit order to sell SMARTBIT at a price
         //    that will overlap with Bob's "activated" call order / margin call.
         //    **Bob should be charged as a maker, and Alice as a taker.**
         //    Alice's limit order should be paritally filled, but Bob's order should be completely filled,
         //    and the debt position should be closed.
         //////
         // Alice obtains her SMARTBIT from Bob
         transfer(bob_id, alice_id, bob_initial_smart);
         BOOST_CHECK_EQUAL(get_balance(bob_id(db), smartbit_id(db)), 0);
         BOOST_CHECK_EQUAL(get_balance(alice_id(db), smartbit_id(db)), bob_initial_smart.amount.value);

         // The margin call should be priced at feed_price / (MSSR-MCFR)
         // where feed_price is expressed as debt / collateral
         // Create a "large" sell order at a "high" price of feed_price * 1.1 = feed_price * (11/10)
         const price alice_order_price_implied = intermediate_feed_price * ratio_type(11, 10);


         const asset alice_debt_to_sell = smartbit.amount(get_balance(alice_id(db), smartbit_id(db)));
         // multiply_and_round_up() handles inverting the price so that the output is in correct collateral units
         const asset alice_collateral_to_buy = alice_debt_to_sell.multiply_and_round_up(alice_order_price_implied);
         limit_order_create_operation alice_sell_op = create_sell_operation(alice_id, alice_debt_to_sell,
                                                                            alice_collateral_to_buy);
         trx.clear();
         trx.operations.push_back(alice_sell_op);
         // asset alice_sell_fee = db.current_fee_schedule().set_fee(trx.operations.back());
         sign(trx, alice_private_key);
         processed_transaction ptx = PUSH_TX(db, trx); // No exception should be thrown
         limit_order_id_type alice_order_id = ptx.operation_results[0].get<object_id_type>();

         // Margin call should exchange all of the available debt (X) for X*(MSSR-MCFR)/feed_price
         // The match price should be the settlement_price/(MSSR-MCFR) = feed_price/(MSSR-MCFR)
         const uint16_t ratio_numerator = current_feed.maximum_short_squeeze_ratio - smartbit_margin_call_fee_ratio;
         BOOST_REQUIRE_EQUAL(ratio_numerator,
                             1450); // GRAPHENE_DEFAULT_MAX_SHORT_SQUEEZE_RATIO - smartbit_margin_call_fee_ratio
         const price expected_match_price = intermediate_feed_price * ratio_type(GRAPHENE_COLLATERAL_RATIO_DENOM,
                                                                           ratio_numerator);
         // Reduces to (17 satoshi SMARTIT / 400 satoshi CORE) * (1000 / 1450)
         // = (17 satoshi SMARTBIT / 400 satoshi CORE) * (100 / 145)
         // = (17 satoshi SMARTBIT / 4 satoshi CORE) * (1 / 145)
         // = 17 satoshi SMARTBIT / 580 satoshi CORE
//         BOOST_CHECK(intermediate_feed_price < initial_feed_price);
         BOOST_CHECK_EQUAL(expected_match_price.base.amount.value, 17); // satoshi SMARTBIT
         BOOST_CHECK_EQUAL(expected_match_price.quote.amount.value, 580); // satoshi CORE

         // Payment to limit order = X*(MSSR-MCFR)/feed_price
         // = 2000000 satoshi SMARTBIT * (580 satoshi CORE / 17 satoshi SMARTBIT)
         // = 68235294.1176 satoshi CORE rounded up to 68235295 satoshi CORE = 682.35295 CORE
         const asset expected_payment_to_alice_core = core.amount(68235295);

         // Expected payment by call order: filled_debt * (MSSR / settlement_price) = filled_debt * (MSSR / feed_price)
         //
         // (MSSR / feed_price) = (1500 / 1000) / (17 satoshi SMARTBIT / 400 satoshi CORE)
         // = (15 / 10) / (17 satoshi SMARTBIT / 400 satoshi CORE)
         // = (15 / 1) / (17 satoshi SMARTBIT / 40 satoshi CORE)
         // = (15 * 40 satoshi CORE) / (17 satoshi SMARTBIT)
         // = (15 * 40 satoshi CORE) / (17 satoshi SMARTBIT)
         // = 600 satoshi CORE / 17 satoshi SMARTBIT
         //
         // Expected payment by call order = 2000000 satoshi SMARTBIT * (600 satoshi CORE / 17 satoshi SMARTBIT)
         // = 2000000 * 600 satoshi CORE / 17
         // = 70588235.2941 satoshi CORE rounding up to 70588236 satoshi CORE = 705.88236 CORE
         const asset expected_payment_from_bob_core = core.amount(70588236);

         // Expected fee = payment by call order - payment to limit order
         // fee = (70588236 - 68235295) satoshi CORE = 2352941 satoshi CORE = 23.52941 CORE
         const asset expected_margin_call_fee =
                 expected_payment_from_bob_core - expected_payment_to_alice_core; // core.amount(2352941);

         // Check Alice's balances
         BOOST_CHECK_EQUAL(get_balance(alice, smartbit), 0);
         BOOST_CHECK_EQUAL(get_balance(alice, core),
                           alice_initial_core.amount.value + expected_payment_to_alice_core.amount.value);

         // Check Alice's limit order is still open
         BOOST_CHECK(!db.find(alice_order_id));

         // Check Bob's debt position is closed
         BOOST_CHECK(!db.find(bob_call_id));

         // Check Bob's balances
         // Bob should have no debt asset
         BOOST_CHECK_EQUAL(get_balance(bob_id(db), smartbit_id(db)), 0);
         // Bob should have collected the balance of his collateral after the margin call
         BOOST_CHECK_EQUAL(get_balance(bob_id(db), core_id(db)),
                           bob_initial_core.amount.value - expected_payment_from_bob_core.amount.value);

         // Check the virtual fill operation on the limit order include reflects the MCFR effect
         graphene::app::history_api hist_api(app);
         flat_set<uint16_t> ops = {0,1,2,3,4}; // Fill operations
         graphene::app::history_operation_detail hist_detail
                 = hist_api.get_account_history_by_operations("alice", ops, 0, 10);
         BOOST_CHECK_EQUAL(hist_detail.total_count, 1);
         vector <operation_history_object> histories = hist_detail.operation_history_objs;
         wdump(("A")(histories));
         for (operation_history_object h : histories) {
            wdump(("B"));
            wdump((h.op));
         }
         wdump(("Z"));


         // Check the asset owner's accumulated asset fees
         BOOST_CHECK(smartbit.dynamic_asset_data_id(db).accumulated_fees == 0);
         BOOST_CHECK(smartbit.dynamic_asset_data_id(db).accumulated_collateral_fees ==
                     expected_margin_call_fee.amount.value);

      } FC_LOG_AND_RETHROW()
   }

BOOST_AUTO_TEST_SUITE_END()