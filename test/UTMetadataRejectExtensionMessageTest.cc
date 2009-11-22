#include "UTMetadataRejectExtensionMessage.h"

#include <iostream>

#include <cppunit/extensions/HelperMacros.h>

#include "BtConstants.h"

namespace aria2 {

class UTMetadataRejectExtensionMessageTest:public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(UTMetadataRejectExtensionMessageTest);
  CPPUNIT_TEST(testGetExtensionMessageID);
  CPPUNIT_TEST(testGetBencodedReject);
  CPPUNIT_TEST(testToString);
  CPPUNIT_TEST(testDoReceivedAction);
  CPPUNIT_TEST_SUITE_END();
public:
  void testGetExtensionMessageID();
  void testGetBencodedReject();
  void testToString();
  void testDoReceivedAction();
};


CPPUNIT_TEST_SUITE_REGISTRATION(UTMetadataRejectExtensionMessageTest);

void UTMetadataRejectExtensionMessageTest::testGetExtensionMessageID()
{
  UTMetadataRejectExtensionMessage msg(1);
  CPPUNIT_ASSERT_EQUAL((uint8_t)1, msg.getExtensionMessageID());
}

void UTMetadataRejectExtensionMessageTest::testGetBencodedReject()
{
  UTMetadataRejectExtensionMessage msg(1);
  msg.setIndex(1);
  CPPUNIT_ASSERT_EQUAL
    (std::string("d8:msg_typei2e5:piecei1ee"), msg.getBencodedData());
}

void UTMetadataRejectExtensionMessageTest::testToString()
{
  UTMetadataRejectExtensionMessage msg(1);
  msg.setIndex(100);
  CPPUNIT_ASSERT_EQUAL(std::string("ut_metadata reject piece=100"),
		       msg.toString());
}

void UTMetadataRejectExtensionMessageTest::testDoReceivedAction()
{
}

} // namespace aria2