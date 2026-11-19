// Results computed by the reference implementation for fixed inputs
// (nonce 00 11 22 .. ff, PIN 12345678, key offset 1, wrap offset 8, account
// 01 23 45 67 89 ab cd ef), printed by tools/oracle/run.sh --vectors. They let
// the unit tests check our cipher on machines that do not have the reference.
#pragma once

#include <string_view>

namespace psvp::test::vectors {

// ps4, computed by the reference
constexpr std::string_view k_ps4_key = "cebcb640080776047b85e85bf350f52d";
constexpr std::string_view k_ps4_wrapped = "882407f7021353615fee35f5dc47dedf";
constexpr std::string_view k_ps4_iv0 = "795cec62674d48c8a6d105c6a2d38a0d";
constexpr std::string_view k_ps4_iv7 = "a69ec89816337f8903986f91236b3e21";
constexpr std::string_view k_ps4_sealed = "238a1bcbc4633b841a20b72e4623579770c66e872a5fdcae75f84a67e39ca08a290e4852d216de51d23afa";
constexpr std::string_view k_ps4_inner = "348e178edb627fb30870b066093058d54bcc16db342cdd9046499d985ac5252420a79e22fa8aee3cd99b52552dac3c4e3ad9d74f72e5cac093ebdd096c27edabde466203dcdcbceefad59937c612c0e8c0c3777606795ea2fb464bd2e1c30959f3ff6b27ff6896f5153587";
// ps5, computed by the reference
constexpr std::string_view k_ps5_key = "d860e146bdb0bd94b460070bb1f3866d";
constexpr std::string_view k_ps5_wrapped = "7bfa185c3a813af3c5b66dfaac28455c";
constexpr std::string_view k_ps5_iv0 = "5a7cb3472823406888911027fe90ff84";
constexpr std::string_view k_ps5_iv7 = "ef853a7618f3bf8ae35d2a3cc99c0ccd";
constexpr std::string_view k_ps5_sealed = "e7f454d21176abcdf689dab3629ba07c4bc5f93d0b489529264a2abcfa5af421c9da002d0e5e4aa69b2f5b";
constexpr std::string_view k_ps5_inner = "f0f058970e77effae4d9ddfb2d88af3ef7ea2f4c6c8784f6990afa6095b5edcf395e98807d67e327dc9d402e14ac321aaeb723196572831eafcd1a28b928f68efd711e266d8fde63a89b895264a5902c89dfdada13286cacbefd412ba86b7279f0a7187f04d2fd7abb78ea";

}  // namespace psvp::test::vectors
